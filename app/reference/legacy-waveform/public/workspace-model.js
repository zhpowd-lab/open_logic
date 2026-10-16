// 展示层的数据约束供浏览器、Worker和宿主测试共用，不参与保护或测量计算。
(function (root) {
  'use strict';
  const PHASES = ['Ia', 'Ib', 'Ic', 'I0'];
  const COLORS = ['#dca515', '#19a9cd', '#d5636d', '#8765bb', '#c38d12', '#218fac', '#c86250', '#758a41'];
  function selection(channels, circuit, voltageGroup = 1) {
    if (![1, 2].includes(voltageGroup) || !Number.isInteger(circuit) || circuit < 1 || circuit > 16)
      throw new Error('请选择第1～16开关和一组公共电压');
    const current = PHASES.map(phase => channels.find(ch => ch.circuit === `SW${circuit}` && ch.phase === phase)?.index);
    const voltage = Array.from({ length: 4 }, (_, i) => ((voltageGroup - 1) * 4 + i) * 2);
    if (current.some(i => !Number.isInteger(i))) throw new Error('开关通道映射不完整');
    return [...current, ...voltage];
  }
  function inferSelection(channels, selected) {
    for (let circuit = 1; circuit <= 16; circuit++) for (const voltage of [1, 2]) {
      const wanted = selection(channels, circuit, voltage);
      if (selected.length === wanted.length && wanted.every(i => selected.includes(i))) return { circuit, voltage };
    }
    return null;
  }
  function validateSnapshot(value) {
    if (!value || value.type !== 'snapshot' || value.channels?.length !== 80 ||
        !Array.isArray(value.selected) || !value.selected.length || value.selected.length > 8 ||
        new Set(value.selected).size !== value.selected.length ||
        value.selected.some(i => !Number.isInteger(i) || i < 0 || i >= 80) ||
        !Array.isArray(value.data) || value.data.length !== value.selected.length + 1)
      throw new Error('波形快照结构无效');
    const x = value.data[0];
    if (!Array.isArray(x) || x.some((v, i) => !Number.isFinite(v) || (i && v <= x[i - 1])) ||
        value.data.slice(1).some(y => !Array.isArray(y) || y.length !== x.length || y.some(v => v !== null && !Number.isFinite(v))))
      throw new Error('波形时间轴或通道长度无效');
    return value;
  }
  function chartData(data, channels, physical) {
    return [data[0], ...channels.map((ch, i) => physical ?
      Array.from(data[i + 1], v => v === null ? null : v * ch.a + ch.b) : data[i + 1])];
  }
  function lowerBound(values, wanted) {
    let lo = 0, hi = values.length;
    while (lo < hi) { const mid = (lo + hi) >>> 1; if (values[mid] < wanted) lo = mid + 1; else hi = mid; }
    return lo;
  }
  // 保留各桶的端点、各通道真实极值和空值边界；不插补、不生成假采样点。
  function envelope(data, limit = 8192) {
    const length = data[0].length;
    if (length <= limit) return data;
    const width = Math.max(1, Math.ceil(length / Math.max(1, Math.floor(limit / (data.length * 2)))));
    const keep = new Set([0, length - 1]);
    for (let start = 0; start < length; start += width) {
      const end = Math.min(length, start + width); keep.add(start); keep.add(end - 1);
      for (let c = 1; c < data.length; c++) {
        let low = -1, high = -1;
        for (let i = start; i < end; i++) {
          if (data[c][i] === null) { if (i === start || data[c][i - 1] !== null) keep.add(i); continue; }
          if (i > start && data[c][i - 1] === null) keep.add(i);
          if (low < 0 || data[c][i] < data[c][low]) low = i;
          if (high < 0 || data[c][i] > data[c][high]) high = i;
        }
        if (low >= 0) { keep.add(low); keep.add(high); }
      }
    }
    const indices = [...keep].sort((a, b) => a - b);
    return data.map(series => indices.map(i => series[i]));
  }
  function session(meta = {}) { return [meta.epoch, meta.generation, meta.timeEpoch, meta.period, meta.enabledChipMask].join(':'); }
  function streamFresh(snapshot, connected, now = Date.now()) {
    const age = now - snapshot.connection?.lastSample;
    return connected && snapshot.status === 'online' && snapshot.meta?.state === 4 && snapshot.meta?.faults === 0 &&
      Number.isFinite(age) && age >= -2000 && age < 5000;
  }
  // 异步健康缓存必须与当前原生会话匹配；缓存过期/跨会话时不能继承旧错误零值。
  function diagnosticHealth(snapshot, now = Date.now()) {
    const h = snapshot.deviceHealth || {}, m = snapshot.meta || {}, age = now - Date.parse(h.checkedAt);
    if (h.online !== true || !Number.isFinite(age) || age < -2000 || age > 45000 ||
        String(h.epoch) !== String(m.epoch) || String(h.capture_period_ticks) !== String(m.period))
      return { fresh: false, fields: {}, owner: null };
    if (h.acquisitionOwner === 'a7_amp') {
      const a = h.a7 || {}, f = a.fields || {};
      if (a.fresh !== true || a.controller_alive !== true || h.acquisitionFresh !== true ||
          String(f.epoch) !== String(m.epoch) || String(f.run_generation) !== String(m.generation) ||
          String(f.period_ticks) !== String(m.period) || String(f.mask) !== String(m.enabledChipMask))
        return { fresh: false, fields: {}, owner: null };
      return { fresh: true, fields: f, owner: 'a7_amp' };
    }
    return { fresh: true, fields: {}, owner: h.acquisitionOwner };
  }
  function decodeHistory(frames) {
    if (!Array.isArray(frames) || !frames.length || frames.length > 131072) throw new Error('原始历史为空或超限');
    const n = frames.length, times = new Float64Array(n), sequences = new Uint32Array(n),
      masks = new Uint8Array(n), valid = new Uint8Array(n), codes = new Int16Array(n * 80);
    const report = { frames: n, channels: 80, gaps: 0, invalid: 0, estimated: 0, unknownQuality: 0,
      min: Array(80).fill(Infinity), max: Array(80).fill(-Infinity) };
    for (let i = 0; i < n; i++) {
      const f = frames[i], bytes = Uint8Array.from(atob(f.raw), c => c.charCodeAt(0));
      if (bytes.length !== 176 || !Number.isInteger(f.n) || f.n < 0 || f.n > 0xffffffff ||
          !Number.isFinite(f.us) || (i && f.us <= times[i - 1])) throw new Error('原始帧长度、序号或时标无效');
      const view = new DataView(bytes.buffer);
      if (view.getUint32(0, true) !== f.n) throw new Error('原始帧序号不匹配');
      const mask = f.enabledChipMask ?? 31;
      if (!Number.isInteger(mask) || mask < 1 || mask > 31) throw new Error('ADC掩码无效');
      if (f.flags !== undefined && (!Number.isInteger(f.flags) || f.flags < 0 || f.flags > 0xffffffff)) throw new Error('采样质量标记无效');
      sequences[i] = f.n; times[i] = f.us; masks[i] = mask;
      valid[i] = f.flags === undefined || ((f.flags & 1) && !(f.flags & (1 << 11))) ? 1 : 0;
      if (i && f.n !== ((sequences[i - 1] + 1) >>> 0)) report.gaps++;
      if (f.flags === undefined) report.unknownQuality++;
      else { if (!(f.flags & 1)) report.invalid++; if (f.flags & (1 << 11)) report.estimated++; }
      for (let c = 0; c < 80; c++) {
        const v = view.getInt16(8 + c * 2, true); codes[i * 80 + c] = v;
        if (masks[i] & (1 << Math.floor(c / 16))) { report.min[c] = Math.min(report.min[c], v); report.max[c] = Math.max(report.max[c], v); }
      }
    }
    report.min = report.min.map(v => Number.isFinite(v) ? v : null);
    report.max = report.max.map(v => Number.isFinite(v) ? v : null);
    return { times, sequences, masks, valid, codes, report };
  }
  function historyView(raw, selected, windowMs, range) {
    const { times, masks, valid, codes, sequences } = raw;
    const first = lowerBound(times, times.at(-1) - windowMs * 1000), origin = times[first];
    const start = range ? Math.max(first, lowerBound(times, origin + range.min * 1000) - 1) : first;
    const end = range ? Math.min(times.length, lowerBound(times, origin + range.max * 1000) + 1) : times.length;
    if (end <= start) return { data: [[], ...selected.map(() => [])], sourceCount: 0, originUs: origin };
    const data = [Array.from(times.subarray(start, end), v => (v - origin) / 1000),
      ...selected.map(c => Array.from({ length: end - start }, (_, j) => valid[j + start] && (masks[j + start] & (1 << Math.floor(c / 16))) ? codes[(j + start) * 80 + c] : null))];
    const display = envelope(data);
    return { data: display, sourceCount: end - start, displayCount: display[0].length,
      decimated: display[0].length < end - start, originUs: origin,
      durationMs: (times.at(-1) - origin) / 1000, firstSequence: sequences[start], lastSequence: sequences[end - 1] };
  }
  const api = { PHASES, COLORS, selection, inferSelection, validateSnapshot, chartData, lowerBound, envelope, session, streamFresh, diagnosticHealth, decodeHistory, historyView };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.WorkspaceModel = api;
})(typeof self === 'object' ? self : globalThis);
