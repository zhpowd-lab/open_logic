const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { execFile } = require('node:child_process');
const { WebSocketServer, WebSocket } = require('ws');
const { CaptureMonitor, exportComtrade } = require('./capture');
const { defaultChannels } = require('./channels');
const { DeviceLink, DeviceHealth } = require('./device-link');
const { exportMonitorState, restoreMonitorState, restoreMonitorSnapshot, LIMIT_BYTES } = require('./capture-state');

const deviceHost = process.env.RT_MPU_DEVICE_IP || '192.168.64.118';
const devicePort = Number(process.env.RT_MPU_WAVE_PORT || 6011);
// 控制台端口与协议由入口配置；当前 periph3 使用 6003 Telnet，auto 仅用于旧配套。
const consolePort = process.env.RT_MPU_CONSOLE_PORT || '6003';
const consoleProtocol = process.env.RT_MPU_CONSOLE_PROTOCOL || 'auto';
const uiPort = Number(process.env.RT_MPU_UI_PORT || 8098);
const directory = path.join(__dirname, 'captures');
const events = [];
// 事件回调只保存电脑观察时间与已有样点事件，每次追加日志并保留最近 100 条展示。
const monitor = new CaptureMonitor(directory, event => {
  events.unshift({ ...event, time: new Date().toISOString() }); events.length = Math.min(events.length, 100);
  fs.mkdirSync(directory, { recursive: true });
  fs.appendFileSync(path.join(directory, 'monitor.jsonl'), JSON.stringify(events[0]) + '\n');
});
const settingsFile = path.join(__dirname, 'settings.json');
if (fs.existsSync(settingsFile)) {
  try {
    const saved = JSON.parse(fs.readFileSync(settingsFile, 'utf8'));
    if (saved.config) monitor.configure(saved.config);
    if (saved.armed === false) monitor.resume();
    if (saved.channels) monitor.setChannels(saved.channels);
    if (saved.selected) monitor.select(saved.selected, saved.windowMs);
  } catch (error) { monitor.onEvent({ type: 'config', text: `本机通道配置未载入：${error.message}` }); }
}
// 显式旧快照交接只恢复已有字段；以后正常退出保存完整原始状态，启动拒绝损坏或跨目标文件。
const runtimeStateFile = path.join(__dirname, 'runtime-state.json');
const restoreFile = process.env.RT_MPU_WAVE_RESTORE_FILE || (fs.existsSync(runtimeStateFile) ? runtimeStateFile : null);
if (restoreFile) {
  if (fs.statSync(restoreFile).size > LIMIT_BYTES * 2) throw new Error('波形运行状态文件超限');
  const saved = JSON.parse(fs.readFileSync(restoreFile, 'utf8'));
  if (saved.format && (!saved.source || saved.source.host !== deviceHost || saved.source.port !== devicePort))
    throw new Error('波形运行状态目标不匹配');
  if (!saved.format && (saved.deviceHost !== deviceHost || saved.devicePort !== devicePort))
    throw new Error('旧波形快照目标不匹配');
  const restored = saved.format ? restoreMonitorState(monitor, saved) : restoreMonitorSnapshot(monitor, saved);
  events.push(...restored.events);
  // 即便完整原码已恢复，跨服务连接边界也必须重新取得连续N点资格；原末点仍用于真实缺口检测。
  monitor.streak = 0; monitor.cycleReadySamples = 0;
  saveSettings();
  // 完整退出状态只消费一次，异常退出后不能反复覆盖更新过的通道配置或累计统计。
  if (restoreFile === runtimeStateFile)
    fs.renameSync(runtimeStateFile, runtimeStateFile + '.restored-' + new Date().toISOString().replace(/[:.]/g, '-'));
  monitor.onEvent({ type: 'migration', text: restored.lossyHistory ?
    '本机网页交接：配置和累计统计保留，旧可见波形已归档；80路原始历史从新包重建，跨重启连续性未知' :
    `本机网页完整恢复 ${restored.historyRestored} 个原始历史点`, lossyHistory: restored.lossyHistory });
}
function saveRuntimeState() {
  const saved = exportMonitorState(monitor, events);
  saved.source = { host: deviceHost, port: devicePort };
  fs.writeFileSync(runtimeStateFile + '.tmp', JSON.stringify(saved));
  fs.renameSync(runtimeStateFile + '.tmp', runtimeStateFile);
}
// 先写临时文件再原子替换本机配置；只保存显示/触发/通道标定，不写装置定值。
function saveSettings() {
  // 原子替换配置文件，断电不会留下只写入一半的通道表。
  fs.writeFileSync(settingsFile + '.tmp', JSON.stringify({ config: monitor.config, armed: monitor.armed, channels: monitor.channels,
    selected: monitor.selected, windowMs: monitor.windowMs }, null, 2));
  fs.renameSync(settingsFile + '.tmp', settingsFile);
}
const files = {
  '/': ['public/index.html','text/html'], '/app.js': ['public/app.js','text/javascript'],
  '/workspace/': ['public/workspace.html','text/html'],
  '/workspace/app.js': ['public/workspace.js','text/javascript'],
  '/workspace/model.js': ['public/workspace-model.js','text/javascript'],
  '/workspace/worker.js': ['public/workspace-worker.js','text/javascript'],
  '/workspace/style.css': ['public/workspace.css','text/css'],
  '/style.css': ['public/style.css','text/css'],
  '/uplot.js': ['node_modules/uplot/dist/uPlot.iife.min.js','text/javascript'],
  '/uplot.css': ['node_modules/uplot/dist/uPlot.min.css','text/css'],
  '/lucide.js': ['node_modules/lucide/dist/umd/lucide.js','text/javascript'],
  '/fft.js': ['public/fft.bundle.js','text/javascript'],
  '/spectrum.js': ['spectrum.js','text/javascript'],
};
// 本机 HTTP 路由提供页面、状态和受限文件下载；只监听回环地址，服务不部署在装置 Linux 上。
const server = http.createServer((request,response) => {
  const url = new URL(request.url, 'http://localhost');
  if (url.pathname === '/favicon.ico') { response.writeHead(204); return response.end(); }
  if (url.pathname === '/api/monitor-state' && request.method === 'GET') {
    // 只在显式请求时导出完整状态；摘要覆盖原始内容，不把网络抽点窗口当成可恢复原始环。
    try {
      const saved = exportMonitorState(monitor, events); saved.source = { host: deviceHost, port: devicePort };
      response.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      return response.end(JSON.stringify(saved));
    } catch (_) { response.writeHead(500); return response.end('MONITOR_STATE_EXPORT_FAILED'); }
  }
  if (url.pathname === '/api/status') {
    response.writeHead(200, {'Content-Type':'application/json'});
    return response.end(JSON.stringify(snapshot()));
  }
  if (url.pathname.startsWith('/captures/')) {
    const name = path.basename(url.pathname);
    if (!/^[\w.-]+\.(cfg|dat|json)$/.test(name)) { response.writeHead(400); return response.end(); }
    const target = path.join(directory, name);
    if (!fs.existsSync(target)) { response.writeHead(404); return response.end(); }
    response.writeHead(200, {'Content-Disposition':`attachment; filename="${name}"`});
    return fs.createReadStream(target).pipe(response);
  }
  const file = files[url.pathname];
  if (!file) { response.writeHead(404); return response.end(); }
  fs.readFile(path.join(__dirname,file[0]),(error,data) => {
    response.writeHead(error ? 404 : 200, {'Content-Type':file[1]}); response.end(error ? '' : data);
  });
});
const clients = new WebSocketServer({ server, maxPayload: 65536 });
// 浏览器命令只影响本机触发/显示/标定/导出；错误回给客户端，清统计需要明确 ACK。
clients.on('connection', client => {
  client.on('message', raw => {
    try {
      const command = JSON.parse(raw.toString());
      if (command.type === 'arm') { monitor.configure(command.config || {}); saveSettings(); }
      else if (command.type === 'resume') { monitor.resume(); saveSettings(); }
      else if (command.type === 'pause') monitor.pause();
      else if (command.type === 'clear_stats') {
        connectionBase = link.connections;
        monitor.clearStats(); client.send(JSON.stringify({ type: 'cleared', startedAt: monitor.stats.startedAt }));
      }
      else if (command.type === 'select') { monitor.select(command.selected, command.windowMs); saveSettings(); }
      else if (command.type === 'profile') {
        monitor.setChannels(command.channels); saveSettings();
        monitor.onEvent({ type: 'config', text: '本机通道名称及换算系数已保存' });
      }
      else if (command.type === 'profile_reset') { monitor.setChannels(defaultChannels()); saveSettings(); }
      else if (command.type === 'export') {
        const name = exportComtrade(directory, monitor.frozen || monitor.recent(monitor.windowMs),
          monitor.frozenMeta || monitor.meta, '手动导出', monitor.channels);
        monitor.onEvent({ type:'saved', text:name, reason:'手动导出' });
        client.send(JSON.stringify({ type: 'exported', name }));
      }
    } catch (error) { client.send(JSON.stringify({ type:'error', text:error.message })); }
  });
});

// 装置端网络录波与浏览器 WebSocket 是两层连接；完整包送监测器，断线只保存现场和重连。
const link = new DeviceLink({ host: deviceHost, port: devicePort,
  onPacket: (meta, frames) => monitor.ingest(meta, frames),
  onDisconnect: reason => monitor.disconnect(reason), onEvent: event => monitor.onEvent(event) });
const healthFile = path.join(directory, `device-health-${deviceHost.replaceAll('.', '-')}.json`);
let previousHealth = {};
try { previousHealth = JSON.parse(fs.readFileSync(healthFile, 'utf8')); } catch (_) { /* 首次运行没有历史启动编号。 */ }
const health = new DeviceHealth(previousHealth, event => monitor.onEvent(event));
// 组合录波监测、TCP连接及只读控制台健康，供 HTTP/WS 共用一致的展示格式。
function snapshot() {
  const connection = link.snapshot();
  return { ...monitor.snapshot(), status: connection.state, lastData: connection.lastPacket,
    connection, deviceHealth: health.value, deviceHost, devicePort, events };
}
let healthBusy = false, shuttingDown = false, healthProcess;
// 串行启动有 12s 上界的只读健康子进程，解析结果并原子保存；忙或退出时跳过，不执行自动重启。
function probeHealth() {
  if (healthBusy || shuttingDown) return;
  healthBusy = true;
  const command = process.env.RT_MPU_PYTHON || (process.platform === 'win32' ? 'py' : 'python3');
  healthProcess = execFile(command, ['-X', 'utf8', path.join(__dirname, '../tools/wave_health.py'), '--ip', deviceHost,
    '--console-port', consolePort, '--console-protocol', consoleProtocol],
    { timeout: 12000, windowsHide: true, maxBuffer: 65536 }, (error, output) => {
      healthBusy = false;
      if (shuttingDown) return;
      let result;
      try { result = error ? { online: false, error: error.killed ? 'PROBE_TIMEOUT' : 'PROBE_FAILED' } : JSON.parse(output); }
      catch (_) { result = { online: false, error: 'PROBE_BAD_REPLY' }; }
      const value = health.update(result);
      fs.mkdirSync(directory, { recursive: true });
      fs.writeFileSync(healthFile + '.tmp', JSON.stringify(value)); fs.renameSync(healthFile + '.tmp', healthFile);
    });
}

// 每 100ms 广播显示快照；缓冲超过 1MiB 的慢浏览器跳过当前刷新，避免拖住持续采集连接。
const broadcast = setInterval(() => {
  monitor.stats.reconnects = Math.max(0, link.connections - connectionBase);
  const message = JSON.stringify(snapshot());
  for (const client of clients.clients) if (client.readyState === WebSocket.OPEN && client.bufferedAmount < 1024*1024) client.send(message);
}, 100);
// 低频追加本机统计和最后健康快照，便于追踪中断/重启，日志时间属于电脑观察时刻。
const statistics = setInterval(() => {
  fs.mkdirSync(directory,{recursive:true});
  fs.appendFileSync(path.join(directory,'statistics.jsonl'), JSON.stringify({
    time:new Date().toISOString(), status:link.state, connection:link.snapshot(), deviceHealth:health.value,
    meta:monitor.meta, stats:monitor.stats,
  })+'\n');
}, 10000);

// 新 TCP 连接在恢复的旧累计数上追加；不能在第一轮广播把旧重连统计清零。
let connectionBase = -monitor.stats.reconnects;
const healthTimer = setInterval(probeHealth, 15000);
// 本机页面监听完成后才启动录波连接与只读健康查询，不发送开出或启动装置应用。
server.listen(uiPort, '127.0.0.1', () => {
  console.log(`RT_MPU waveform http://127.0.0.1:${uiPort} device=${deviceHost}:${devicePort}`);
  link.start(); probeHealth();
});
// 幂等停止本机链接、健康进程、定时器及所有 WS 客户端，释放 HTTP 服务；不改装置运行状态。
function shutdown() {
  if (shuttingDown) return;
  shuttingDown = true;
  try { saveRuntimeState(); } catch (error) { console.error('波形运行状态保存失败：' + error.message); }
  link.stop(); healthProcess?.kill();
  clearInterval(broadcast); clearInterval(statistics); clearInterval(healthTimer);
  for (const client of clients.clients) client.terminate();
  clients.close(); server.close();
}
process.on('SIGINT', shutdown); process.on('SIGTERM', shutdown);
