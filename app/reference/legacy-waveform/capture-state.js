const crypto = require('node:crypto');
const { RECORD, MAX_SAMPLES } = require('./capture');
const { validateChannels } = require('./channels');

// 状态文件只用于本机升级交接；摘要校验防止半写和意外损坏，不充当网络认证。
const FORMAT = 'rt_mpu_wave_monitor', LIMIT_BYTES = 128 * 1024 * 1024;
function fail() { throw new Error('波形运行状态无效，原状态未改变'); }
function object(value) { if (!value || typeof value !== 'object' || Array.isArray(value)) fail(); return value; }
function integer(value, maximum = Number.MAX_SAFE_INTEGER) {
  if (!Number.isSafeInteger(value) || value < 0 || value > maximum) fail(); return value;
}
function number(value) { if (!Number.isFinite(value) || value < 0) fail(); return value; }
function bool(value) { if (typeof value !== 'boolean') fail(); return value; }
function clone(value) { return JSON.parse(JSON.stringify(value)); }
function validMeta(value) {
  const meta = clone(object(value));
  for (const key of ['epoch', 'generation', 'period', 'state', 'faults', 'enabledChipMask', 'timeEpoch', 'timeQuality'])
    if (meta[key] !== undefined) integer(meta[key], 0xffffffff);
  if (meta.timeQuality !== undefined) integer(meta.timeQuality, 3);
  if (meta.rtcSource !== undefined) integer(meta.rtcSource, 0xffffffff);
  if (meta.tickHz !== undefined && meta.tickHz !== 24000000) fail();
  if (meta.enabledChipMask !== undefined && (!meta.enabledChipMask || (meta.enabledChipMask & ~31))) fail();
  return meta;
}
function encodeFrame(frame) { return frame ? { ...frame, raw: frame.raw.toString('base64') } : null; }
function decodeFrame(value) {
  const frame = clone(object(value)); integer(frame.n, 0xffffffff); number(frame.us);
  if (typeof frame.raw !== 'string' || !/^[A-Za-z0-9+/]+={0,2}$/.test(frame.raw)) fail();
  const raw = Buffer.from(frame.raw, 'base64');
  if (raw.length !== RECORD || raw.toString('base64') !== frame.raw || raw.readUInt32LE(0) !== frame.n) fail();
  frame.raw = raw;
  if (frame.enabledChipMask !== undefined && (!integer(frame.enabledChipMask, 31))) fail();
  if (frame.flags !== undefined) integer(frame.flags, 0xffffffff);
  if (frame.receivedAt !== undefined) number(frame.receivedAt);
  if (frame.utcSeconds !== undefined && (typeof frame.utcSeconds !== 'string' || !/^\d{1,20}$/.test(frame.utcSeconds) ||
      BigInt(frame.utcSeconds) > 0xffffffffffffffffn)) fail();
  if (frame.utcNanoseconds !== undefined) integer(frame.utcNanoseconds, 999999999);
  if (frame.timeEpoch !== undefined) integer(frame.timeEpoch, 0xffffffff);
  if (frame.timeQuality !== undefined) integer(frame.timeQuality, 3);
  if ((frame.flags & 2) && (frame.utcSeconds === undefined || frame.utcNanoseconds === undefined ||
      !frame.timeEpoch || !frame.timeQuality || BigInt(frame.utcSeconds) < 946684800n || BigInt(frame.utcSeconds) >= 4102444800n)) fail();
  return frame;
}
function frames(value, maximum = MAX_SAMPLES) {
  if (!Array.isArray(value) || value.length > maximum) fail(); return value.map(decodeFrame);
}
function stats(value, template) {
  const result = clone(object(value));
  for (const key of Object.keys(template)) {
    if (key === 'startedAt') { if (typeof result[key] !== 'string' || !Number.isFinite(Date.parse(result[key]))) fail(); }
    else if (['minUs', 'irqMax'].includes(key) && result[key] === null) continue;
    else if (['minUs', 'maxUs', 'sumUs'].includes(key)) number(result[key]);
    else integer(result[key]);
  }
  return result;
}
function candidateFor(monitor, value) {
  const next = new monitor.constructor(monitor.directory);
  next.configure(object(value.config)); next.setChannels(validateChannels(value.channels));
  next.select(value.selected, value.windowMs); next.armed = bool(value.armed);
  next.meta = validMeta(value.meta); next.stats = stats(value.stats, next.emptyStats());
  return next;
}
function install(monitor, next) {
  for (const key of ['history', 'written', 'config', 'channels', 'selected', 'windowMs', 'armed', 'meta', 'stats',
    'dropBase', 'skipInterval', 'irqPeak', 'capture', 'frozen', 'frozenMeta', 'streak', 'previous', 'faultKey',
    'resumeOnData', 'cycleReadySamples', 'migration']) monitor[key] = next[key];
}

// 完整导出保留原始80路、活动后录、人工冻结、统计起点及触发连续资格；从不从展示抽点反推原码。
function exportMonitorState(monitor, events = []) {
  const count = Math.min(monitor.written, MAX_SAMPLES), history = [];
  for (let i = monitor.written - count; i < monitor.written; i++) history.push(encodeFrame(monitor.history[i % MAX_SAMPLES]));
  const state = { config: monitor.config, channels: monitor.channels, selected: monitor.selected, windowMs: monitor.windowMs,
    armed: monitor.armed, meta: monitor.meta, stats: monitor.stats, written: monitor.written, history,
    dropBase: monitor.dropBase, skipInterval: monitor.skipInterval, irqPeak: monitor.irqPeak,
    capture: monitor.capture ? { ...monitor.capture, frames: monitor.capture.frames.map(encodeFrame) } : null,
    frozen: monitor.frozen?.map(encodeFrame) ?? null, frozenMeta: monitor.frozenMeta,
    streak: monitor.streak, previous: encodeFrame(monitor.previous), faultKey: monitor.faultKey,
    resumeOnData: monitor.resumeOnData, cycleReadySamples: monitor.cycleReadySamples,
    migration: monitor.migration, events: clone(events) };
  const payload = JSON.stringify(state);
  if (Buffer.byteLength(payload) > LIMIT_BYTES) fail();
  return { version: 1, format: FORMAT, savedAt: new Date().toISOString(),
    sha256: crypto.createHash('sha256').update(payload).digest('hex'), payload };
}

// 先在隔离对象验证全部状态和摘要，再一次替换；拒绝部分恢复后误丢原有人工暂停或统计。
function restoreMonitorState(monitor, envelope) {
  object(envelope);
  if (envelope.version !== 1 || envelope.format !== FORMAT || typeof envelope.payload !== 'string' ||
      Buffer.byteLength(envelope.payload) > LIMIT_BYTES || typeof envelope.sha256 !== 'string' ||
      crypto.createHash('sha256').update(envelope.payload).digest('hex') !== envelope.sha256) fail();
  const value = JSON.parse(envelope.payload), next = candidateFor(monitor, value);
  const history = frames(value.history), written = integer(value.written);
  if (history.length !== Math.min(written, MAX_SAMPLES)) fail();
  next.history = new Array(MAX_SAMPLES); next.written = written;
  history.forEach((frame, i) => { next.history[(written - history.length + i) % MAX_SAMPLES] = frame; });
  next.frozen = value.frozen === null ? null : frames(value.frozen);
  next.frozenMeta = value.frozenMeta === null ? null : validMeta(value.frozenMeta);
  if (!!next.frozen !== !!next.frozenMeta) fail();
  next.previous = value.previous === null ? null : decodeFrame(value.previous);
  if (history.length) {
    const tail = history[history.length - 1], previous = next.previous;
    if (!previous || !previous.raw.equals(tail.raw) ||
        ['n', 'us', 'flags', 'enabledChipMask', 'utcSeconds', 'utcNanoseconds', 'timeEpoch', 'timeQuality']
          .some(key => previous[key] !== tail[key])) fail();
    if (!next.meta.period || next.meta.tickHz !== 24000000) fail();
  }
  next.capture = null;
  if (value.capture !== null) {
    const c = object(value.capture), captured = frames(c.frames);
    if (!captured.length || next.armed) fail();
    next.capture = { ...clone(c), frames: captured, meta: validMeta(c.meta), channels: validateChannels(c.channels) };
    number(c.endUs); number(c.triggerUs);
    if (c.endUs < c.triggerUs || c.triggerUs < captured[0].us || !captured.some(frame => frame.us === c.triggerUs)) fail();
    if (c.remainingSamples !== null && c.remainingSamples !== undefined) {
      if (!integer(c.remainingSamples, MAX_SAMPLES)) fail();
      const detail = object(c.meta.triggerDetails), requested = integer(detail.requestedPostSamples, MAX_SAMPLES);
      const triggerIndex = captured.findIndex(frame => frame.us === c.triggerUs);
      if (requested - (captured.length - triggerIndex - 1) !== c.remainingSamples) fail();
      for (let i = triggerIndex; i < captured.length; i++) {
        const frame = captured[i];
        if (!(frame.flags & 1) || (frame.flags & 2048) ||
            (i > triggerIndex && (frame.n !== ((captured[i-1].n + 1) >>> 0) ||
              Math.abs(frame.us - captured[i-1].us - c.meta.period / 24) > 2))) fail();
      }
    }
  }
  next.streak = integer(value.streak); next.cycleReadySamples = integer(value.cycleReadySamples);
  // 原始历史可精确恢复，跨进程的连续触发资格必须重新取得，不能只信任保存的N计数。
  next.streak = 0; next.cycleReadySamples = 0;
  next.dropBase = integer(value.dropBase, 0xffffffff); next.skipInterval = bool(value.skipInterval);
  next.resumeOnData = bool(value.resumeOnData);
  if (typeof value.faultKey !== 'string' || value.faultKey.length > 128) fail(); next.faultKey = value.faultKey;
  next.irqPeak = value.irqPeak === null ? null : clone(object(value.irqPeak));
  next.migration = value.migration === null ? null : clone(object(value.migration));
  if (!Array.isArray(value.events) || value.events.length > 100) fail();
  const events = clone(value.events); events.forEach(object);
  install(monitor, next);
  return { events, historyRestored: history.length, lossyHistory: false, qualificationReset: true };
}

// 旧服务只能提供展示快照；仅无人工冻结/活动录波时迁移已知配置与累计统计，并明确原始环重建。
function restoreMonitorSnapshot(monitor, snapshot) {
  object(snapshot);
  if (snapshot.type !== 'snapshot' || snapshot.frozen !== false || snapshot.capturing !== false ||
      snapshot.captureMeta !== null || snapshot.resumeOnData !== false) fail();
  const next = candidateFor(monitor, snapshot);
  if (!Array.isArray(snapshot.events) || snapshot.events.length > 100) fail();
  const events = clone(snapshot.events); events.forEach(object);
  next.dropBase = Math.max(0, integer(snapshot.meta.drops || 0, 0xffffffff) - integer(snapshot.streamDrops || 0));
  next.irqPeak = snapshot.irqPeak === null ? null : clone(object(snapshot.irqPeak));
  next.skipInterval = true; next.previous = null; next.cycleReadySamples = 0;
  next.migration = { restoredAt: new Date().toISOString(), lossyHistory: true, rawHistoryRestored: 0,
    note: '旧服务仅可见通道已单独归档；完整80路原始历史不可得，新有效包重建历史，不补零。' };
  install(monitor, next);
  return { events, historyRestored: 0, lossyHistory: true };
}
module.exports = { exportMonitorState, restoreMonitorState, restoreMonitorSnapshot, LIMIT_BYTES };
