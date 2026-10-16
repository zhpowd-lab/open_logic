const net = require('node:net');
const { PacketDecoder } = require('./capture');

// 电脑到装置录波 TCP 的连接生命周期；区分未连通、心跳正常无样点与采集故障，仅重连不重启装置。
class DeviceLink {
  // 注入地址、包/中断/事件回调和重试超时，保存本机连接统计，初态停止等待 start。
  constructor({ host, port, onPacket, onDisconnect, onEvent, retryMs = 1500, timeoutMs = 5000 }) {
    this.host = host; this.port = port;
    this.onPacket = onPacket; this.onDisconnect = onDisconnect; this.onEvent = onEvent;
    this.retryMs = retryMs; this.timeoutMs = timeoutMs;
    this.state = 'connecting'; this.lastPacket = 0; this.lastSample = 0; this.lastFrame = null;
    this.connections = 0; this.disconnects = 0; this.attempts = 0;
    this.lastError = ''; this.nextRetry = 0; this.outage = false; this.stopped = true;
  }
  // 返回本轮连接与最近包/样点到达时间，供页面区分 TCP 在线和数据是否推进。
  snapshot() {
    return { state: this.state, lastPacket: this.lastPacket, lastSample: this.lastSample,
      connections: this.connections, disconnects: this.disconnects, attempts: this.attempts,
      lastError: this.lastError, nextRetry: this.nextRetry };
  }
  // 幂等启用连接生命周期，只在停止态启动一次连接尝试。
  start() { if (this.stopped) { this.stopped = false; this.connect(); } }
  // 禁止后续重连，清计时器并关闭本 socket；不操作装置程序或供电。
  stop() {
    this.stopped = true; clearTimeout(this.retry); clearInterval(this.sampleWatch);
    this.socket?.destroy();
  }
  // 建立独立解码器与 socket，维护心跳/样点超时并有界延迟重连；同一中断只记一次现场。
  connect() {
    if (this.stopped) return;
    this.attempts++; this.nextRetry = 0;
    let failure = '', received = false, connectedAt = 0, freshSamples = false;
    const socket = net.createConnection({ host: this.host, port: this.port });
    this.socket = socket;
    // 完整包回调区分心跳与实际样点推进；仅 TCP 可连或有空心跳不能显示采样在线。
    const decoder = new PacketDecoder((meta, frames) => {
      if (this.stopped) return;
      this.lastPacket = Date.now(); received = true;
      const newest = frames.at(-1), previous = this.lastFrame;
      const session = `${meta.epoch}:${meta.generation}:${meta.timeEpoch ?? ''}`;
      // 包到达不等于采集推进；重复序号/时标或明确无效/估算的末点不刷新实际样点时间。
      const valid = newest && (newest.flags === undefined || ((newest.flags & 1) && !(newest.flags & (1 << 11))));
      const advance = previous && ((newest?.n - previous.n) >>> 0);
      const progressed = valid && (!previous || session !== previous.session ||
        (newest.us > previous.us && advance > 0 && advance < 0x80000000));
      if (progressed) {
        this.lastSample = this.lastPacket; freshSamples = true;
        this.lastFrame = { session, n: newest.n, us: newest.us };
      }
      // 连续诊断的历史错误由独立遥测显示；流中的 active fault 仍表示真实停止，不能忽略。
      const state = meta.state !== 4 || meta.faults ? 'fault' :
        this.lastPacket - Math.max(freshSamples ? this.lastSample : 0, connectedAt) >= this.timeoutMs ? 'stalled' :
        freshSamples ? 'online' : 'waiting';
      if (this.outage) {
        this.onEvent({ type: 'reconnected', text: '波形连接已恢复' }); this.outage = false;
      }
      if (state === 'stalled' && this.state !== 'stalled')
        this.onEvent({ type: 'stalled', text: '波形连接仍在，采样点停止更新' });
      this.state = state; this.lastError = '';
      this.onPacket(meta, frames);
    });
    socket.setTimeout(this.timeoutMs);
    socket.setKeepAlive(true, 1000);
    // 连接成功先处于等待，另以样点到达时间检查停滞，不拿 keepalive 当采样证明。
    socket.on('connect', () => {
      connectedAt = Date.now(); this.connections++; this.state = 'waiting';
      this.sampleWatch = setInterval(() => {
        if (this.state === 'online' && Date.now() - this.lastSample >= this.timeoutMs) {
          this.state = 'stalled';
          this.onEvent({ type: 'stalled', text: '波形连接仍在，采样点停止更新' });
        }
      }, Math.min(1000, this.timeoutMs));
    });
    // 每个 TCP chunk 交增量解码器；格式/质量校验抛错时关闭当前连接并留下具体原因。
    socket.on('data', chunk => {
      try { decoder.push(chunk); } catch (error) {
        failure = 'PROTOCOL: ' + error.message;
        this.onEvent({ type: 'protocol', text: failure }); socket.destroy();
      }
    });
    // socket 无数据超时只结束连接，后续 close 统一安排重连，不复位装置。
    socket.on('timeout', () => {
      failure = connectedAt ? 'DATA_TIMEOUT: 波形端口未返回数据' : 'CONNECT_TIMEOUT: 波形端口连接超时';
      socket.destroy();
    });
    socket.on('error', error => { failure = error.code || error.message; });
    // 关闭回调清当前监测计时器，真实中断保存一次；连续重试失败不重复计断线。
    socket.on('close', () => {
      clearInterval(this.sampleWatch);
      if (this.stopped) return;
      this.lastError = failure || 'PEER_CLOSED: 装置关闭波形连接';
      this.state = 'offline';
      // 失败的重连不是新的断线，同一中断只保存一次现场。
      if (!this.outage) {
        this.outage = true;
        if (received) { this.disconnects++; this.onDisconnect(this.lastError); }
        else this.onEvent({ type: 'connection_failed', text: `波形未连接：${this.lastError}` });
      }
      this.nextRetry = Date.now() + this.retryMs;
      this.retry = setTimeout(() => this.connect(), this.retryMs);
    });
  }
}

// 控制台只读健康结果汇总器；保留上次离线快照时间，bootId 变化才计重启，不把失联当复位。
class DeviceHealth {
  // 注入历史健康快照与事件输出，在线状态先标未知，等待本次控制台核对。
  constructor(previous = {}, onEvent = () => {}) {
    // 历史启动编号用于识别重启，online 先未知；离线保存的字段不能冒充本次已测数据。
    this.value = { ...previous, online: null }; this.onEvent = onEvent;
  }
  // 接收本次健康结果，在线时只保留当前采集字段，离线时保留上次现场；事件反映连接变化而非复位原因。
  update(result) {
    const previous = this.value;
    // 在线快照只展示本次取得的采集字段，切换应用或探针不能继承旧计数。
    const value = { ...(result.online ? {
      reboots: previous.reboots || 0, lastRebootAt: previous.lastRebootAt,
    } : previous), ...result, checkedAt: new Date().toISOString() };
    if (result.online) {
      value.error = '';
      value.lastOnlineAt = value.checkedAt;
      if (previous.bootId && previous.bootId !== result.bootId) {
        value.reboots = (previous.reboots || 0) + 1;
        value.lastRebootAt = value.checkedAt;
        this.onEvent({ type: 'device_reboot', text: '检测到装置重新启动，复位原因待查',
          previousBootId: previous.bootId, bootId: result.bootId, uptimeSeconds: result.uptimeSeconds });
      }
      if (previous.online === false)
        this.onEvent({ type: 'console_recovered', text: '装置控制台已恢复连接', bootId: result.bootId });
    } else if (previous.online !== false) {
      this.onEvent({ type: 'console_unreachable', text: '装置控制台暂时无法连接，串口状态尚未核对',
        reason: result.error });
    }
    // 控制台失联时保留上次快照，界面必须同时展示快照的读取时间。
    this.value = value;
    return value;
  }
}

module.exports = { DeviceLink, DeviceHealth };
