const fs = require('node:fs');
const path = require('node:path');
const { defaultChannels, validateChannels, measure, channelValid } = require('./channels');

// 录波网络头与原始记录长度属于 RWF 传输协议；导出时另组标准 COMTRADE DAT，二者不能混用。
const HEADER = 64, RECORD = 176, MAX_SAMPLES = 131072, DISPLAY_LIMIT = 8192, FFT_LIMIT = 4096;

// TCP 录波流增量解码器；buffer 保存未满包尾部，onPacket 只接收已验证的完整 RWF1/2/3 包。
class PacketDecoder {
  // 注入完整包接收回调并建立空尾包缓冲；不自行连接装置。
  constructor(onPacket) { this.buffer = Buffer.alloc(0); this.onPacket = onPacket; }
  // 追加任意 TCP 分段，核查版本/长度/时钟与逐点时标后交付完整包；坏包抛错由连接层断开重连。
  push(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= HEADER) {
      const b = this.buffer;
      const version = b.readUInt32LE(4), header = b.readUInt32LE(8), recordBytes = version === 3 ? 192 : RECORD;
      if (b.readUInt32LE(0) !== 0x31465752 || ![1, 2, 3].includes(version) ||
          header !== ({ 1: HEADER, 2: 96, 3: 128 }[version]) || b.readUInt32LE(12) !== recordBytes ||
          b.readUInt32LE(16) > 64 || b.readUInt32LE(60) !== 24000000)
        throw new Error('录波流头格式不符');
      const count = b.readUInt32LE(16), size = header + count * recordBytes;
      if (b.length < size) return;
      // 整包元数据：tick/period/IRQ 单位为 24 MHz tick；epoch/generation/掩码限定同一次采集会话。
      const meta = {
        epoch: b.readUInt32LE(20), simulatedPeak: b.readUInt32LE(24),
        generation: b.readUInt32LE(28), tick: Number(b.readBigUInt64LE(32)),
        period: b.readUInt32LE(40), drops: b.readUInt32LE(44),
        state: b.readUInt32LE(48), faults: b.readUInt32LE(52),
        irqMax: b.readUInt32LE(56), tickHz: b.readUInt32LE(60), version, enabledChipMask: 0x1f,
      };
      if (version >= 2) {
        Object.assign(meta, { irqBatchMax: b.readUInt32LE(64), irqPeakSequence: b.readUInt32LE(68),
          irqPeakCounter: b.readUInt32LE(72), irqValidCount: b.readUInt32LE(76),
          irqPeakTicks: b.readBigUInt64LE(80).toString(), irqGe3usCount: b.readUInt32LE(88),
          irqUnknownCount: b.readUInt32LE(92) });
        if (meta.irqValidCount + meta.irqUnknownCount !== count ||
            meta.irqGe3usCount > meta.irqValidCount ||
            (meta.irqValidCount && meta.irqBatchMax >= meta.period)) throw new Error('IRQ 诊断计数无效');
      }
      if (version === 3) {
        // 旧 RWF3 的保留字段为零，继续按全五片解释；新版本携带真实有效掩码。
        meta.enabledChipMask = b.readUInt32LE(108) || 0x1f;
        if (meta.enabledChipMask & ~0x1f) throw new Error('ADC 有效掩码无效');
        Object.assign(meta, { timeEpoch: b.readUInt32LE(96), timeQuality: b.readUInt32LE(100),
          rtcSource: b.readUInt32LE(104), irqPeakUtcSeconds: b.readBigUInt64LE(112).toString(),
          irqPeakUtcNanoseconds: b.readUInt32LE(120), irqPeakTimeFlags: b.readUInt32LE(124) });
        if (meta.timeQuality > 3 || meta.irqPeakUtcNanoseconds >= 1000000000)
          throw new Error('RTC 时标无效');
      }
      const frames = [];
      for (let i = 0; i < count; i++) {
        const offset = header + i * recordBytes;
        const raw = Buffer.from(b.subarray(offset, offset + RECORD));
        // 逐点结构：n 是采样序号，us 是板端单调采样时间，raw 保留 ADC 码；UTC/flags 在 RWF3 另附。
        const frame = { n: raw.readUInt32LE(0), us: meta.tick / 24 + raw.readUInt32LE(4), raw,
          enabledChipMask: meta.enabledChipMask };
        if (version === 3) {
          Object.assign(frame, { utcSeconds: b.readBigUInt64LE(offset + 176).toString(),
            utcNanoseconds: b.readUInt32LE(offset + 184), flags: b.readUInt32LE(offset + 188),
            timeEpoch: meta.timeEpoch, timeQuality: meta.timeQuality });
          if (frame.utcNanoseconds >= 1000000000 || ((frame.flags & 2) &&
              (!meta.timeEpoch || !meta.timeQuality || BigInt(frame.utcSeconds) < 946684800n ||
               BigInt(frame.utcSeconds) >= 4102444800n))) throw new Error('样点 RTC 时标无效');
        }
        frames.push(frame);
      }
      this.buffer = b.subarray(size);
      this.onPacket(meta, frames);
    }
    if (this.buffer.length > 128 + 64 * 192) throw new Error('录波接收积压超限');
  }
}

// 只有 UTC_VALID 样点才格式化九位纳秒 UTC；无效/旧流返回 null，不能用电脑时间冒充板端时间。
function sampleTime(frame) {
  if (!frame || !(frame.flags & 2)) return null;
  return new Date(Number(frame.utcSeconds) * 1000).toISOString().slice(0, 19) +
    '.' + String(frame.utcNanoseconds).padStart(9, '0') + 'Z';
}

// 将样点 UTC 与可选相对偏移换成 COMTRADE 六位微秒时间，整数纳秒运算保留跨秒进位。
function comtradeTime(frame, offsetUs = 0) {
  let ns = BigInt(frame.utcSeconds) * 1000000000n + BigInt(frame.utcNanoseconds) + BigInt(Math.round(offsetUs * 1000));
  const d = new Date(Number(ns / 1000000n));
  return `${String(d.getUTCDate()).padStart(2,'0')}/${String(d.getUTCMonth()+1).padStart(2,'0')}/${d.getUTCFullYear()},` +
    d.toISOString().slice(11,19) + '.' + String((ns % 1000000000n) / 1000n).padStart(6, '0');
}

// 以原始有符号码导出标准 BINARY CFG/DAT，并用 JSON 保存逐点质量/时钟来源；拒绝混合 ADC 掩码。
function exportComtrade(directory, frames, meta, reason, channels = defaultChannels(), triggerUs = frames[0]?.us) {
  if (!frames.length) throw new Error('没有可导出的样点');
  const enabledChipMask = frames[0].enabledChipMask ?? meta.enabledChipMask ?? 0x1f;
  if (!enabledChipMask || (enabledChipMask & ~0x1f) ||
      frames.some(frame => (frame.enabledChipMask ?? enabledChipMask) !== enabledChipMask))
    throw new Error('录波包含不同的 ADC 通道配置');
  const capturedChannels = channels.filter(ch => enabledChipMask & (1 << Math.floor(ch.index / 16)));
  const recordBytes = 8 + capturedChannels.length * 2 + 8;
  fs.mkdirSync(directory, { recursive: true });
  const date = new Date(), name = date.toISOString().replace(/[:.]/g, '-') + '-' + frames[0].n;
  const triggerFrame = frames.find(frame => frame.us === triggerUs);
  // 有效 UTC 使用 E907 样点时钟；旧流仍明确标记为电脑接收时间估计。
  const firstDate = new Date((frames.at(-1).receivedAt || date.getTime()) - (frames.at(-1).us - frames[0].us) / 1000);
  // 旧流没有板端 UTC 时，按电脑接收时刻估计绝对日期；导出 JSON 必须明确时间来源。
  const stamp = d => `${String(d.getUTCDate()).padStart(2,'0')}/${String(d.getUTCMonth()+1).padStart(2,'0')}/${d.getUTCFullYear()},${d.toISOString().slice(11,23)}000`;
  const cfg = [`RT_MPU_${meta.simulatedPeak ? 'SIMULATED' : 'ADC'},E907,1999`,
    `${capturedChannels.length + 64},${capturedChannels.length}A,64D`];
  capturedChannels.forEach((ch, i) => cfg.push(`${i+1},${ch.name},${ch.phase},${ch.circuit},${ch.unit},${ch.a},${ch.b},0,-32768,32767,1,1,S`));
  for (let i = 0; i < 64; i++) cfg.push(`${i+1},DI${i+1},,,0`);
  cfg.push('50','1',`${meta.period ? 24000000/meta.period : 6400},${frames.length}`,
    sampleTime(frames[0]) ? comtradeTime(frames[0]) : stamp(firstDate),
    sampleTime(frames[0]) ? (sampleTime(triggerFrame) ? comtradeTime(triggerFrame) :
      comtradeTime(frames[0], triggerUs - frames[0].us)) :
      stamp(new Date(firstDate.getTime() + (triggerUs - frames[0].us) / 1000)), 'BINARY','1');
  const dat = Buffer.alloc(recordBytes * frames.length);
  frames.forEach((frame, i) => {
    const offset = i * recordBytes;
    dat.writeUInt32LE(i + 1, offset);
    dat.writeUInt32LE(Math.max(0, Math.round(frame.us - frames[0].us)), offset + 4);
    capturedChannels.forEach((ch, j) => dat.writeInt16LE(frame.raw.readInt16LE(8 + ch.index * 2), offset + 8 + j * 2));
    frame.raw.copy(dat, offset + 8 + capturedChannels.length * 2, 168, RECORD);
  });
  fs.writeFileSync(path.join(directory, name + '.cfg'), cfg.join('\r\n') + '\r\n');
  fs.writeFileSync(path.join(directory, name + '.dat'), dat);
  // DAT 保持标准格式；逐点质量放在 JSON，旧流缺失 flags 时明确记为未知。
  const adcValidFlag = 1, adcEstimatedFlag = 1 << 11;
  const sampleFlags = frames.map(frame => Number.isInteger(frame.flags) &&
    frame.flags >= 0 && frame.flags <= 0xffffffff ? frame.flags : null);
  const estimatedSequences = frames.filter((_, index) => sampleFlags[index] !== null &&
    (sampleFlags[index] & adcEstimatedFlag)).map(frame => frame.n);
  fs.writeFileSync(path.join(directory, name + '.json'), JSON.stringify({
    ...meta, reason, sampleCount: frames.length, hostCaptureTime: date.toISOString(),
    timeSource: sampleTime(frames[0]) ? 'E907 software RTC initialized from Linux; UTC; PPS not locked' :
      'E907 monotonic; absolute date estimated from host receipt; UTC/PPS unverified',
    firstSampleTime: sampleTime(frames[0]), lastSampleTime: sampleTime(frames.at(-1)),
    triggerSampleTime: sampleTime(triggerFrame),
    sampleTimeEpoch: frames[0].timeEpoch ?? null,
    timestampResolutionNote: 'UTC 字段为纳秒格式，24 MHz 计数分辨率约 41.667 ns；不是绝对对时精度保证',
    analogStorage: 'signed raw ADC code; engineering value = CFG a * code + b',
    enabledChipMask, channels: capturedChannels,
    sampleQuality: {
      flagsByRecord: sampleFlags,
      adcValidFlag, adcEstimatedFlag,
      trueAdcSamples: sampleFlags.filter(flags => flags !== null &&
        (flags & adcValidFlag) && !(flags & adcEstimatedFlag)).length,
      estimatedSamples: estimatedSequences.length, estimatedSequences,
      unknownQualitySamples: sampleFlags.filter(flags => flags === null).length,
      invalidQualitySamples: sampleFlags.filter(flags => flags !== null &&
        (flags & adcValidFlag) && (flags & adcEstimatedFlag)).length,
      note: 'flagsByRecord 与 DAT 记录一一对应；估算点不等于真实 ADC，null 表示旧流未传质量标记',
    },
    digitalNote: '前三个数字字保持最近有效值，第四字保留；首个有效读数前状态未知',
  }, null, 2));
  return name;
}

// 电脑端历史环与触发状态机；冻结只固定显示/录波，后台计数继续，原始/估算质量明确保留。
class CaptureMonitor {
  // 建立固定容量历史环、触发配置与冻结/连续状态；directory 保存录波，onEvent 注入事件输出。
  constructor(directory, onEvent = () => {}) {
    this.directory = directory; this.onEvent = onEvent;
    // history 是固定容量环，written 为累计游标；冻结另持完整记录，不能让后台覆盖正在导出的窗口。
    this.history = new Array(MAX_SAMPLES); this.written = 0;
    // 触发阈值使用原始 ADC 码；电平/周波偏差要求连续N点，沿触发一次，周波比较固定配置50Hz。
    this.config = { channel: 16, threshold: 100, condition: 'abs_gt', preMs: 100, postMs: 100, consecutive: 3,
      referenceCycles: 1, postCycles: 10, postMode: 'ms' };
    this.meta = {}; this.stats = this.emptyStats();
    this.channels = defaultChannels(); this.selected = [16, 18, 20, 22];
    this.windowMs = 500; this.dropBase = 0; this.skipInterval = false; this.irqPeak = null;
    // armed/capture/frozen 表示布防、后触发记录与冻结显示；resumeOnData 只属于网络中断自动冻结。
    this.armed = true; this.capture = null; this.frozen = null; this.frozenMeta = null; this.streak = 0; this.previous = null;
    this.faultKey = ''; this.resumeOnData = false; this.cycleReadySamples = 0;
    this.migration = null;
  }
  // 创建本次监测统计窗口，未知最小时长及 IRQ 峰值保持 null，不能把缺字段显示为零。
  emptyStats() {
    return { frames: 0, gaps: 0, intervals: 0, minUs: null, maxUs: 0, sumUs: 0, reconnects: 0,
      irqMax: null, irqValid: 0, irqGe3us: 0, irqUnknown: 0,
      rtcSamples: 0, rtcRollovers: 0, rtcErrors: 0, startedAt: new Date().toISOString() };
  }
  // 只清运行统计与窗口峰值，保留冻结/触发条件；跳过清零边界间隔但仍检测序号缺口。
  clearStats() {
    this.stats = this.emptyStats(); this.irqPeak = null; this.dropBase = this.meta.drops || 0;
    this.skipInterval = true;
    this.onEvent({ type: 'clear', text: '运行统计已清零', epoch: this.meta.epoch });
  }
  // 验证整张 80 路名称/单位/线性系数配置后替换；不改变装置 ADC 或保护定值。
  setChannels(channels) { this.channels = validateChannels(channels); }
  // 核查 1～8 个显示通道与窗口长度并去重；只是上位机显示选择，不改变硬件采集掩码。
  select(selected, windowMs = this.windowMs) {
    if (!Array.isArray(selected) || selected.length < 1 || selected.length > 8 ||
        selected.some(ch => !Number.isInteger(ch) || ch < 0 || ch >= 80) ||
        !Number.isFinite(windowMs) || windowMs < 20 || windowMs > 10000) throw new Error('显示通道或时间窗口无效');
    this.selected = [...new Set(selected)]; this.windowMs = windowMs;
  }
  // 冻结最近显示窗口并取消自动恢复，后台 ingest 继续；人工暂停不能被网络重连解除。
  pause() { this.resumeOnData = false; if (!this.frozen && this.written) { this.frozen = this.recent(this.windowMs); this.frozenMeta = { ...this.meta }; } }
  // 结束未完成后触发记录后切回连续显示，取消布防/冻结；不控制装置采集启停。
  resume() {
    if (this.capture) this.finish('手动结束后触发记录');
    this.armed = false; this.frozen = this.frozenMeta = null; this.streak = 0; this.resumeOnData = false;
    this.cycleReadySamples = 0;
  }
  // 合并并验证阈值、方向、连续次数与前后窗口，重新布防并优先显示触发通道。
  configure(config) {
    const c = { ...this.config, ...config };
    if (!Number.isInteger(c.channel) || c.channel < 0 || c.channel >= 80 ||
        !Number.isFinite(c.threshold) || c.threshold < -32768 || c.threshold > (c.condition === 'cycle_diff' ? 65535 : 32767) ||
        !['abs_gt','gt','lt','rising','falling','cycle_diff'].includes(c.condition) ||
        (c.condition === 'cycle_diff' && c.threshold < 0) ||
        ![1, 2].includes(c.referenceCycles) ||
        !['ms', 'cycles'].includes(c.postMode) ||
        !Number.isInteger(c.postCycles) || c.postCycles < 0 || c.postCycles > 50 ||
        (c.condition === 'abs_gt' && c.threshold < 0) ||
        !Number.isInteger(c.consecutive) || c.consecutive < 1 || c.consecutive > 128 ||
        !Number.isFinite(c.preMs) || c.preMs < 0 || c.preMs > 1000 ||
        !Number.isFinite(c.postMs) || c.postMs < 0 || c.postMs > 1000)
      throw new Error('触发参数超出范围');
    this.config = c; this.armed = true; this.capture = this.frozen = this.frozenMeta = null; this.streak = 0; this.resumeOnData = false;
    this.cycleReadySamples = 0;
    this.selected = [c.channel, ...this.selected.filter(ch => ch !== c.channel)].slice(0, 8);
  }
  // 按已写游标从固定容量历史环取有序窗口，再按单调采样时间筛选，避免用网络到达时间当间隔。
  recent(ms = 1000) {
    const count = Math.min(this.written, MAX_SAMPLES), frames = [];
    for (let i = this.written - count; i < this.written; i++) frames.push(this.history[i % MAX_SAMPLES]);
    if (!frames.length) return frames;
    return frames.filter(f => f.us >= frames[frames.length-1].us - ms * 1000);
  }
  // 处理完整包会话/RTC变更、质量与连续性，更新历史和统计，再判阈值/后触发；断线恢复只解除自动冻结。
  ingest(meta, frames) {
    // 断线自动冻结在有效样点恢复后解除；手动暂停和阈值触发继续保留。
    if (this.resumeOnData && frames.length && meta.state === 4 && !meta.faults) {
      this.frozen = this.frozenMeta = null; this.resumeOnData = false;
    }
    // 空心跳没有采样周期；同一会话沿用最近周期，避免把重连等待误判成改频。
    if (!frames.length && !meta.period && meta.epoch === this.meta.epoch &&
        meta.generation === this.meta.generation)
      meta = { ...meta, period: this.meta.period, timeEpoch: this.meta.timeEpoch,
        timeQuality: this.meta.timeQuality, rtcSource: this.meta.rtcSource,
        enabledChipMask: this.meta.enabledChipMask };
    if (frames.length && this.meta.timeEpoch !== undefined &&
        (meta.timeEpoch !== this.meta.timeEpoch || meta.timeQuality !== this.meta.timeQuality)) {
      if (this.capture) this.finish('RTC 校时或有效状态变化，结束旧时间代次录波');
      this.onEvent({ type: 'clock', text: `RTC 时间代次 ${this.meta.timeEpoch} → ${meta.timeEpoch}`,
        sampleTime: sampleTime(frames[0]) });
      this.written = 0; this.streak = 0; this.cycleReadySamples = 0;
    }
    if (this.meta.epoch && (meta.epoch !== this.meta.epoch ||
        meta.generation !== this.meta.generation || meta.period !== this.meta.period ||
        meta.enabledChipMask !== this.meta.enabledChipMask)) {
      if (this.capture) this.finish('E907 会话或采样配置变化，后触发数据不完整');
      this.onEvent({ type: 'session', text: 'E907 会话或采样配置变化' });
      this.previous = null; this.written = 0; this.capture = null; this.streak = 0; this.cycleReadySamples = 0;
      this.dropBase = 0;
    }
    this.meta = meta;
    if (meta.version >= 2) {
      this.stats.irqValid += meta.irqValidCount; this.stats.irqGe3us += meta.irqGe3usCount;
      this.stats.irqUnknown += meta.irqUnknownCount;
      if (meta.irqValidCount && (this.stats.irqMax === null || meta.irqBatchMax > this.stats.irqMax)) {
        this.stats.irqMax = meta.irqBatchMax;
        this.irqPeak = { sequence: meta.irqPeakSequence, counter: meta.irqPeakCounter,
          ticks: meta.irqPeakTicks, latencyTicks: meta.irqBatchMax, epoch: meta.epoch,
          generation: meta.generation, observedAt: new Date().toISOString(), timeEpoch: meta.timeEpoch,
          sampleTime: sampleTime({ flags: meta.irqPeakTimeFlags, utcSeconds: meta.irqPeakUtcSeconds,
            utcNanoseconds: meta.irqPeakUtcNanoseconds }) };
        this.onEvent({ type: 'irq_peak', text: `IRQ 峰值 ${(meta.irqBatchMax/24).toFixed(3)} μs，样点 ${meta.irqPeakSequence}`,
          ...this.irqPeak });
      }
    } else this.stats.irqUnknown += frames.length;
    const faultKey = meta.state === 4 && !meta.faults ? '' : `${meta.state}:${meta.faults}`;
    if (faultKey && faultKey !== this.faultKey) {
      this.onEvent({ type:'fault', text:`E907 状态 ${meta.state}，故障位 ${meta.faults}`,
        sampleTime: sampleTime(frames[0] || this.previous), timeNote: '首个受影响样点；无帧时为最后有效样点' });
      if (this.capture) this.finish('E907 故障，后触发数据不完整');
      else if (this.written && !this.frozen) {
        this.frozen = this.recent();
        this.frozenMeta = { ...meta };
        const name = exportComtrade(this.directory, this.frozen, this.frozenMeta, 'E907 故障', this.channels);
        this.onEvent({ type:'saved', text:name });
      }
    }
    this.faultKey = faultKey;
    for (const frame of frames) {
      frame.receivedAt = Date.now();
      let rtcConsistent = true;
      if (sampleTime(frame)) {
        this.stats.rtcSamples++;
        if (sampleTime(this.previous) && frame.timeEpoch === this.previous.timeEpoch) {
          const delta = (BigInt(frame.utcSeconds) - BigInt(this.previous.utcSeconds)) * 1000000000n +
            BigInt(frame.utcNanoseconds - this.previous.utcNanoseconds);
          const monotonicNs = BigInt(Math.round((frame.us - this.previous.us) * 1000));
          // 网络相对时标为整数微秒，检查允许两侧量化误差，不把量化当成 RTC 抖动。
          if (delta <= 0n || delta - monotonicNs > 2000n || monotonicNs - delta > 2000n) {
            this.stats.rtcErrors++; rtcConsistent = false;
            this.onEvent({ type: 'rtc_error', text: '样点 RTC 与单调计数不一致',
              sampleTime: sampleTime(frame), previousSampleTime: sampleTime(this.previous),
              deltaNs: delta.toString(), monotonicNs: monotonicNs.toString() });
          }
          if (frame.utcSeconds !== this.previous.utcSeconds) this.stats.rtcRollovers++;
        }
      }
      let continuous = true;
      if (this.previous) {
        continuous = frame.n === ((this.previous.n + 1) >>> 0) && frame.us > this.previous.us;
        if (!continuous) {
          this.stats.gaps++; this.streak = 0; this.cycleReadySamples = 0;
          this.onEvent({ type: 'gap', text: `样点不连续 ${this.previous.n} → ${frame.n}`,
            sampleTime: sampleTime(frame), previousSampleTime: sampleTime(this.previous) });
          if (this.capture) this.finish('触发期间样点不连续');
        } else if (!this.skipInterval) {
          const delta = frame.us - this.previous.us;
          this.stats.intervals++; this.stats.sumUs += delta;
          this.stats.minUs = this.stats.minUs === null ? delta : Math.min(delta, this.stats.minUs);
          this.stats.maxUs = Math.max(delta, this.stats.maxUs);
        }
      }
      this.skipInterval = false;
      const value = frame.raw.readInt16LE(8 + this.config.channel * 2);
      const previousValue = this.previous?.raw.readInt16LE(8 + this.config.channel * 2);
      const t = this.config.threshold;
      // 先按采集掩码排除补零通道，再按原始码判阈值；工程量显示系数不影响此触发。
      let hit = channelValid(frame, this.config.channel) && { abs_gt: Math.abs(value) > t, gt: value > t, lt: value < t,
        rising: previousValue !== undefined && previousValue <= t && value > t,
        falling: previousValue !== undefined && previousValue >= t && value < t }[this.config.condition];
      // 周波比较只使用真实、连续 ADC 点；整数点数来自实际周期，不按网络包或浏览器刷新计数。
      const points = meta.period > 0 ? meta.tickHz / meta.period / 50 : 0;
      const cyclePoints = Number.isInteger(points) && points > 0 && points * 2 < MAX_SAMPLES ? points : 0;
      let difference = null, reference = null;
      const stepUs = meta.period / 24;
      const validCycleFrame = Number.isInteger(frame.flags) && frame.flags >= 0 && frame.flags <= 0xffffffff &&
        (frame.flags & 1) && !(frame.flags & (1 << 11)) && channelValid(frame, this.config.channel) &&
        meta.state === 4 && !meta.faults && rtcConsistent && continuous &&
        (!this.previous || Math.abs(frame.us - this.previous.us - stepUs) <= 2);
      if (this.config.condition === 'cycle_diff') {
        const valid = validCycleFrame;
        if (!valid || !continuous || (this.previous && Math.abs(frame.us - this.previous.us - stepUs) > 2))
          this.cycleReadySamples = 0;
        const offset = cyclePoints * this.config.referenceCycles;
        if (valid && cyclePoints && this.cycleReadySamples >= offset) {
          reference = this.history[(this.written - offset) % MAX_SAMPLES];
          if (reference && reference.n === ((frame.n - offset) >>> 0) &&
              Math.abs(frame.us - reference.us - offset * stepUs) <= 2) {
            difference = Math.abs(value - reference.raw.readInt16LE(8 + this.config.channel * 2));
          }
        }
        hit = difference !== null && difference > t;
        if (valid) this.cycleReadySamples++;
      } else this.cycleReadySamples = 0;
      this.history[this.written++ % MAX_SAMPLES] = frame;
      this.stats.frames++;
      if (this.capture && this.capture.remainingSamples !== null && this.capture.remainingSamples !== undefined && !validCycleFrame)
        this.finish('周波后触发样点质量或时标异常，数据不完整');
      if (this.capture) {
        this.capture.frames.push(frame);
        if (this.capture.remainingSamples !== null && this.capture.remainingSamples !== undefined) {
          this.capture.remainingSamples--;
          if (this.capture.remainingSamples <= 0) this.finish(this.capture.meta.triggerDetails?.condition === 'cycle_diff' ? '周波差值触发' : '阈值触发');
        } else if (frame.us >= this.capture.endUs) this.finish('阈值触发');
      } else if (this.armed && continuous && meta.state === 4 && !meta.faults) {
        this.streak = hit ? this.streak + 1 : 0;
        const required = ['rising','falling'].includes(this.config.condition) ? 1 : this.config.consecutive;
        if (this.streak >= required) {
          this.armed = false;
          const cycle = this.config.condition === 'cycle_diff';
          const cyclePost = cycle || this.config.postMode === 'cycles';
          if (cyclePost && (!cyclePoints || !validCycleFrame)) { this.armed = true; this.streak = 0; this.previous = frame; continue; }
          const postSamples = cyclePost ? cyclePoints * this.config.postCycles : null;
          const triggerDetails = { condition: this.config.condition, channel: this.config.channel, threshold: t,
            consecutive: required, referenceCycles: cycle ? this.config.referenceCycles : null,
            referenceSequence: reference?.n ?? null, difference, cyclePoints: cycle ? cyclePoints : null,
            postCycles: cyclePost ? this.config.postCycles : null, requestedPostSamples: postSamples };
          this.capture = { frames: this.recent(this.config.preMs),
            endUs: frame.us + (cyclePost ? this.config.postCycles * 20000 : this.config.postMs * 1000),
            remainingSamples: postSamples, triggerUs: frame.us,
            meta: { ...meta, triggerDetails }, channels: this.channels.map(ch => ({ ...ch })) };
          this.onEvent({ type: 'trigger', text: cycle ?
            `ADC 通道 ${this.config.channel+1}，与前 ${this.config.referenceCycles} 周波偏差 ${difference} 码，连续 ${required} 点` :
            `ADC 通道 ${this.config.channel+1}，原始码 ${value}`,
            sampleTime: sampleTime(frame), sequence: frame.n, timeEpoch: frame.timeEpoch, ...triggerDetails });
          if (cyclePost ? postSamples === 0 : !this.config.postMs) this.finish(cycle ? '周波差值触发' : '阈值触发');
        }
      }
      this.previous = frame;
    }
  }
  // 结束当前触发记录，按触发时配置导出并冻结；reason 保留不完整或故障原因。
  finish(reason) {
    if (!this.capture) return;
    const c = this.capture; this.capture = null; this.frozen = c.frames; this.frozenMeta = c.meta;
    const name = exportComtrade(this.directory, c.frames, c.meta, reason, c.channels, c.triggerUs);
    this.onEvent({ type: 'saved', text: name, reason });
  }
  // 只在真实连接中断时保存最后有效窗口并记事件，清连续性锚点；不复位或发送开出命令。
  disconnect(reason = '') {
    this.onEvent({ type: 'disconnect', text: '波形连接中断' + (reason ? `：${reason}` : ''), sampleTime: sampleTime(this.previous),
      timeNote: '最后有效样点，断线观察时刻另记' });
    if (this.capture) this.finish('连接中断，后触发数据不完整');
    else if (this.written && !this.frozen) {
      this.resumeOnData = true;
      this.frozen = this.recent();
      this.frozenMeta = { ...this.meta };
      const name = exportComtrade(this.directory, this.frozen, this.frozenMeta, '连接中断', this.channels);
      this.onEvent({ type:'saved', text:name });
    }
    this.previous = null; this.streak = 0; this.written = 0; this.cycleReadySamples = 0;
  }
  // 构造显示数据、工程量统计、质量/连接诊断快照；未采集通道返回 null，冻结元数据与当前状态分开。
  snapshot() {
    const frames = this.frozen || this.recent(this.windowMs), selected = this.selected;
    // 长窗显示保留每桶所有已选通道极值；完整原始环和录波不抽点，FFT 使用另一路均匀原始尾窗。
    const displayFrames = envelopeFrames(frames, selected, DISPLAY_LIMIT);
    const origin = frames[0]?.us || 0;
    const data = displayData(displayFrames, selected, origin);
    const fftFrames = frames.slice(-FFT_LIMIT);
    const fftEligible = !fftFrames.some((frame, i) =>
      (frame.flags !== undefined && (!(frame.flags & 1) || (frame.flags & (1 << 11)))) ||
      (i && (frame.n !== ((fftFrames[i-1].n + 1) >>> 0) || frame.us <= fftFrames[i-1].us)));
    const fftData = displayData(fftEligible ? fftFrames : [], selected, origin);
    const historyFrames = this.recent(10000);
    const historyDurationMs = historyFrames.length > 1 ? (historyFrames.at(-1).us - historyFrames[0].us) / 1000 : 0;
    return { type: 'snapshot', data, fftData, selected, sourceSampleCount: frames.length, displaySampleCount: displayFrames.length,
      displayDecimated: displayFrames.length < frames.length, historyCapacity: MAX_SAMPLES, historyDurationMs,
      availableWindowMs: Math.min(this.windowMs, frames.length > 1 ? (frames.at(-1).us - frames[0].us) / 1000 : 0),
      migration: this.migration, meta: this.meta, captureMeta: this.frozenMeta, stats: this.stats,
      channels: this.channels, windowMs: this.windowMs, measurements: selected.map(ch => measure(frames, this.channels[ch])),
      digital: frames.length ? Array.from({ length: 64 }, (_, i) => i < 48 ?
        (frames.at(-1).raw.readUInt16LE(168 + Math.floor(i / 16) * 2) >> (i % 16)) & 1 : null) : [],
      irqPeak: this.irqPeak, firstSampleTime: sampleTime(frames[0]), lastSampleTime: sampleTime(frames.at(-1)),
      latestSampleTime: sampleTime(this.previous), streamDrops: Math.max(0, (this.meta.drops || 0) - this.dropBase),
      config: this.config, armed: this.armed, frozen: !!this.frozen, capturing: !!this.capture,
      resumeOnData: this.resumeOnData, captureProgress: this.capture ? { endUs: this.capture.endUs,
        remainingSamples: this.capture.remainingSamples ?? null, triggerUs: this.capture.triggerUs } : null,
      averageUs: this.stats.intervals ? this.stats.sumUs/this.stats.intervals : null };
  }
}

// 将已选通道的有效原始码转换为展示数组，时域与 FFT 共用相同单位和掩码处理。
function displayData(frames, selected, origin) {
  const data = [[], ...selected.map(() => [])];
  for (const frame of frames) {
    data[0].push((frame.us - origin) / 1000);
    selected.forEach((ch, j) => data[j+1].push(channelValid(frame, ch) ? frame.raw.readInt16LE(8 + ch * 2) : null));
  }
  return data;
}

// 每桶保存边界及各通道最小/最大点；限制网络展示量，同时不漏掉短脉冲极值。
function envelopeFrames(frames, selected, limit) {
  if (frames.length <= limit) return frames;
  const buckets = Math.max(1, Math.floor((limit - 2) / (selected.length * 2 + 2)));
  const indices = new Set([0, frames.length - 1]);
  for (let bucket = 0; bucket < buckets; bucket++) {
    const start = Math.floor(bucket * frames.length / buckets);
    const end = Math.floor((bucket + 1) * frames.length / buckets);
    indices.add(start); indices.add(end - 1);
    for (const channel of selected) {
      let min = Infinity, max = -Infinity, minIndex = start, maxIndex = start;
      for (let i = start; i < end; i++) if (channelValid(frames[i], channel)) {
        const value = frames[i].raw.readInt16LE(8 + channel * 2);
        if (value < min) { min = value; minIndex = i; }
        if (value > max) { max = value; maxIndex = i; }
      }
      indices.add(minIndex); indices.add(maxIndex);
    }
  }
  return [...indices].sort((a, b) => a - b).map(index => frames[index]);
}

module.exports = { PacketDecoder, CaptureMonitor, exportComtrade, sampleTime, RECORD, MAX_SAMPLES };
