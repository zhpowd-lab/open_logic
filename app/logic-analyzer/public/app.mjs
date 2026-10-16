import { MAX_BYTES, MAX_SAMPLES, CLOCK_HZ, decodeCapture, normalizeMetadata, findEdge, centeredWindow, cursorAt, overviewBuckets } from './capture-core.mjs';

const DEVICE_REFRESH_MS = 200; // 只决定网页读取已提交数据的频率。

const $ = id => document.getElementById(id);
const colors = ['#55dec1','#7ed6bd','#82b6f2','#a5a3f2','#df9cde','#eba9a9','#edbe73','#c6d783','#70cad7','#7eade5','#a7a2de','#ce9ed4','#ddac98','#d2c583','#8cc598','#8faec5'];
const lightColors = ['#007e68','#287d59','#286ea9','#6756a8','#9b509d','#a44d59','#986007','#687a19','#197384','#386da8','#695aa0','#925b99','#98603e','#817019','#3c7d4c','#4b708e'];
const laneColor = channel => (document.documentElement.dataset.theme === 'light' ? lightColors : colors)[channel];
const state = {
  samples: null, metadata: null, view: { start: 499968, end: 500480 },
  cursors: { A: null, B: null }, tool: 'A', status: 'loading', running: false,
  position: 0, available: 0, pendingTrigger: -1, trigger: -1, scanFrom: 1,
};
let animation = 0, drawQueued = false, drag = null, overviewCache = null;
let sourceGeneration = 0;
let deviceTimer = 0, displayGeneration = 0;
let monitorTimer = 0, monitorGeneration = 0, monitoring = false;
const MONITOR_CHANNELS = 16; // 当前值使用独立指示灯，避免与波形游标混淆。
const MONITOR_WORD_MAX = 0xffff;
const MONITOR_STALE_MS = 3000; // 服务仍在线也不能把裸核停止的旧值标为实时。

function nsText(ns) {
  if (ns === null || !Number.isFinite(ns)) return 'unknown';
  const magnitude = Math.abs(ns);
  if (magnitude >= 1e9) return `${+(ns / 1e9).toFixed(6)} s`;
  if (magnitude >= 1e6) return `${+(ns / 1e6).toFixed(6)} ms`;
  if (magnitude >= 1000) return `${+(ns / 1000).toFixed(6)} µs`;
  return `${+ns.toFixed(3)} ns`;
}
const wordText = word => `0x${word.toString(16).padStart(4, '0').toUpperCase()}`;
const numberText = value => value === null ? 'unknown' : value.toLocaleString('en-US');
const span = () => state.view.end - state.view.start;
const sampleTime = index => state.metadata?.samplePeriodNs === null ? `#${Math.round(index)}` : nsText(index * state.metadata.samplePeriodNs);

function setStatus(status, text) {
  state.status = status;
  $('state').dataset.state = status; $('state').textContent = text;
  const loaded = !!state.samples;
  const busy = status === 'loading';
  $('run').disabled = !loaded || busy || state.running || (!state.metadata?.deviceConnected && state.position >= state.samples.length);
  $('stop').disabled = !state.running;
  $('single').disabled = !loaded || busy || !state.samples.length;
  $('restart').disabled = !loaded || busy || state.running || !state.samples.length;
  for (const id of ['triggerChannel', 'triggerEdge', 'triggerFrom', 'triggerOnRun']) $(id).disabled = state.running;
}

function notice(text, warning = false) {
  $('notice').textContent = text; $('notice').classList.toggle('warning', warning);
}

function stopDisplay(status = 'stopped', text = '已停止 · 显示冻结') {
  state.running = false; cancelAnimationFrame(animation);
  clearTimeout(deviceTimer); displayGeneration++;
  setStatus(status, text); scheduleDraw();
}

function setView(start, end, unrestricted = false) {
  if (!state.samples) return;
  const width = Math.max(2, Math.min(state.samples.length * 2, end - start));
  if (!unrestricted) start = Math.max(-width / 2, Math.min(state.samples.length - width / 2, start));
  state.view = { start, end: start + width }; scheduleDraw();
}

function updateCursors() {
  if (!state.samples) return;
  for (const name of ['A', 'B']) {
    const point = state.cursors[name] === null ? null : cursorAt(state.samples.subarray(0, state.available), state.metadata, state.cursors[name]);
    const target = $(`readout${name}`);
    if (!point) { target.textContent = `${name}  未放置 / 无样本`; delete target.dataset.index; delete target.dataset.word; continue; }
    target.textContent = `${name}  ${nsText(point.timeNs)}${state.metadata.nominalTime || state.metadata.droppedSamples > 0 ? '（名义）' : ''}  ·  索引 ${numberText(point.index)}\n${wordText(point.word)}  ·  ${point.word.toString(2).padStart(16, '0')}\n序号 ${numberText(point.sequence)}`;
    target.dataset.index = point.index; target.dataset.word = point.word;
  }
  const { A, B } = state.cursors;
  $('delta').textContent = A === null || B === null || A >= state.available || B >= state.available ? 'Δt —' :
    `Δ样本 ${Math.abs(B - A)} · Δt ${state.metadata.samplePeriodNs === null ? 'unknown' : nsText(Math.abs(B - A) * state.metadata.samplePeriodNs)}`;
}

function updateMetrics() {
  const m = state.metadata;
  $('period').textContent = nsText(m.samplePeriodNs);
  $('tickPeriod').textContent = m.samplePeriodTicks === null ? (m.samplePeriodNs === null ? '读取速率 unknown' : `${numberText(1e9 / m.samplePeriodNs)} 次 / s`) :
    `${m.samplePeriodTicks} tick @ 24 MHz · ${+(1e9 / m.samplePeriodNs / 1e6).toFixed(6)} MHz`;
  $('sampleCount').textContent = numberText(m.sampleCount);
  $('rawSize').textContent = `${numberText(m.sampleCount * 2)} B · ${m.samplePeriodNs === null ? '时长 unknown' : nsText(m.sampleCount * m.samplePeriodNs)}`;
  $('dropped').textContent = numberText(m.droppedSamples);
  $('sequence').textContent = `首序号 ${numberText(m.sequenceStart)}`;
  $('sourceName').textContent = m.source ?? '本地原始文件 · 来源 unknown';
  $('sourceBadge').textContent = m.deviceConnected ? `${m.softwareServiceTest ? '服务软件测试' : m.pattern ? 'CPU3 软件图案' : '装置数据总线'} · ${m.captureState}` : m.demo ? '固定测试图案 · 本机回放' : '本地文件 · 本机回放';
  $('sourceModeNote').textContent = m.softwareServiceTest ? '当前为独立服务的软件测试，未采集实物数据。停止只冻结网页。' : m.deviceConnected ? 'CPU3 独立采集；时间轴按名义周期，实际读入口间隔见装置信息。停止只冻结网页。' : '当前为本机回放，停止只冻结网页。';
  $('devicePanel').hidden = !m.deviceConnected;
  $('restart').hidden = !!m.deviceConnected;
  if (m.deviceConnected) {
    const labels = { idle: '待采集', running: '采集中', full: '满深度，保留首段', stopped: '采集已停', fault: '采集故障，保留首段', off: 'CPU3 已关闭' };
    $('deviceStatus').textContent = `上次读取：${labels[m.captureState] ?? m.captureState} · 段 ${m.captureId} · 错误码 ${m.errorCode}`;
    $('deviceTiming').textContent = `读入口间隔 ${m.minIntervalTicks ?? 'unknown'}～${m.maxIntervalTicks ?? 'unknown'} tick；最大读取 ${m.maxReadTicks ?? 'unknown'} tick、迟到 ${m.maxLateTicks ?? 'unknown'} tick。正式采样前准备 ${m.primingReads ?? 'unknown'} 次，最大 ${m.primingMaxTicks ?? 'unknown'} tick，不计入样本段。统计覆盖本次尝试；1 tick ≈ 41.667 ns。`;
  }
  $('showNotch').hidden = !m.demo;
  $('triggerFrom').max = Math.max(0, state.samples.length - 1);
  $('cursorIndex').max = Math.max(0, state.samples.length - 1);
  $('overviewExtent').textContent = `0 → ${sampleTime(state.samples.length)} · ${numberText(state.samples.length)} 点`;
}

function installCapture(capture, extra = {}) {
  stopDisplay();
  state.samples = capture.samples;
  state.metadata = { ...capture.metadata, ...extra };
  state.position = 0; state.available = state.samples.length;
  state.trigger = -1; state.pendingTrigger = -1;
  state.cursors = { A: null, B: null }; overviewCache = null;
  $('triggerResult').textContent = '尚未搜索触发';
  $('triggerFrom').value = '0';
  $('cursorIndex').value = Math.max(0, Math.min(500248, state.samples.length - 1));
  state.view = state.metadata.demo ? { start: 499968, end: 500480 } : { start: 0, end: Math.max(2, state.samples.length || 8192) };
  updateMetrics(); setStatus('ready', '已载入 · 显示停止');
  const monitorAvailable = !!state.metadata.deviceConnected && state.metadata.liveMonitorSupported === true;
  $('livePanel').hidden = !monitorAvailable;
  if (monitorAvailable) startMonitor();
  else stopMonitor();
  const m = state.metadata;
  notice(m.deviceConnected ? '装置段已载入，显示保持停止。运行读取已提交样本；新建装置采集会明确替换装置段，满深度不循环。' : m.depthLimited ? `源数据超过深度上限：只保留前 ${numberText(m.sampleCount)} 点，后 ${numberText(m.excludedSamples)} 点未载入。已停止，不循环覆盖。` :
    m.droppedSamples > 0 ? `源报告丢样 ${numberText(m.droppedSamples)}；缺失位置未知，时间轴只表示保留样本的名义时间，不补点。` :
      '本机连续样本回放。停止只暂停显示刷新；运行从回放进度继续，满深度后保留原段。', m.depthLimited || m.droppedSamples > 0);
  scheduleDraw();
}

function stopMonitor() {
  monitoring = false;
  clearTimeout(monitorTimer);
  monitorGeneration++;
  $('monitorToggle').textContent = '继续监视';
}

function startMonitor() {
  if (monitoring || !state.metadata?.liveMonitorSupported) return;
  monitoring = true;
  const generation = ++monitorGeneration;
  let lastKey = '', lastProgress = performance.now();
  $('monitorToggle').textContent = '暂停监视';
  async function poll() {
    try {
      const response = await fetch('/api/capture', { signal: AbortSignal.timeout(5000) });
      if (!response.ok) throw new Error('装置监视连接异常，保留上次读数');
      const info = await response.json();
      if (!monitoring || generation !== monitorGeneration) return;
      const key = `${info.sourceEpoch}/${info.liveReadSequence}`;
      if (key !== lastKey || info.captureState === 'running') {
        lastKey = key;
        lastProgress = performance.now();
      } else if (info.liveMonitorEnabled && performance.now() - lastProgress > MONITOR_STALE_MS) {
        throw new Error('CPU3 读数计数已停止推进，上次读数已过期');
      }
      const valid = Number.isInteger(info.liveWord) && info.liveWord >= 0 && info.liveWord <= MONITOR_WORD_MAX;
      $('monitorWord').textContent = valid ? wordText(info.liveWord) : 'unknown';
      $('monitorWord').dataset.word = valid ? info.liveWord : '';
      for (let channel = 0; channel < MONITOR_CHANNELS; channel++) {
        const bit = valid ? String((info.liveWord >> channel) & 1) : '?';
        const indicator = $('monitorBits').children[channel];
        indicator.dataset.value = bit;
        indicator.querySelector('output').textContent = bit;
      }
      $('monitorStatus').dataset.sequence = info.liveReadSequence ?? '';
      $('monitorStatus').textContent = info.captureState === 'running' ? '正在采集波形段，实时监视暂时保持前值' :
        info.liveErrorCode ? `实时读取故障，错误码 ${info.liveErrorCode}；已停止总线重试` :
        info.liveMonitorEnabled ? `监视中 · 读数计数 ${numberText(info.liveReadSequence ?? null)} · ${new Date().toLocaleTimeString()}` : '等待 CPU3 实时监视就绪';
    } catch (error) {
      if (!monitoring || generation !== monitorGeneration) return;
      $('monitorStatus').textContent = error.message;
    }
    if (monitoring && generation === monitorGeneration) monitorTimer = setTimeout(poll, DEVICE_REFRESH_MS);
  }
  poll();
}

async function serviceCapture() {
  const response = await fetch('/api/capture', { signal: AbortSignal.timeout(5000) });
  if (!response.ok) throw new Error((await response.json().catch(() => null))?.error ?? '服务元数据读取失败');
  const info = await response.json();
  const query = info.deviceConnected ? `?${new URLSearchParams({ capture: info.captureId, epoch: info.sourceEpoch, count: info.sampleCount })}` : '';
  let capture;
  if (info.deviceConnected && info.sampleCount === 0) {
    capture = { samples: new Uint16Array(0), metadata: { ...normalizeMetadata(info), sampleCount: 0, depthLimited: false, excludedSamples: 0 } };
  } else {
    const data = await fetch(`/api/samples${query}`, { signal: AbortSignal.timeout(5000) });
    if (!data.ok) throw new Error((await data.json().catch(() => null))?.error ?? '服务原码读取失败');
    capture = decodeCapture(new Uint8Array(await data.arrayBuffer()), info);
  }
  return { capture, info };
}

async function loadService() {
  const generation = ++sourceGeneration;
  stopDisplay('loading', '正在加载本机数据');
  try {
    const { capture, info } = await serviceCapture();
    if (generation !== sourceGeneration) return;
    installCapture(capture, { ...info, demo: info.demo, sourceSampleCount: info.sourceSampleCount,
      depthLimited: info.depthLimited, excludedSamples: info.excludedSamples });
  } catch (error) {
    if (generation !== sourceGeneration) return;
    stopDisplay('error', '加载失败 · 已停止'); notice(error.message, true);
  }
}

async function loadLocalFile() {
  const file = $('rawFile').files[0], meta = $('metaFile').files[0];
  if (!file) { notice('请选择小端 uint16 原始样本文件。', true); return; }
  if (!file.size || file.size % 2) { notice('文件长度必须为非零偶数，不能保留半条样本。', true); return; }
  const generation = ++sourceGeneration;
  stopDisplay();
  try {
    if (meta && meta.size > 65536) throw new Error('元数据 JSON 超过 64 KiB');
    const [buffer, input] = await Promise.all([file.slice(0, MAX_BYTES).arrayBuffer(), meta ? meta.text().then(text => JSON.parse(text.replace(/^\uFEFF/, ''))) : {}]);
    if (generation !== sourceGeneration) return;
    installCapture(decodeCapture(new Uint8Array(buffer), input, file.size / 2));
  } catch (error) { if (generation === sourceGeneration) notice(`载入失败，原段仍保留：${error.message}`, true); }
}

function triggerOptions() {
  const from = Number($('triggerFrom').value);
  if (!Number.isSafeInteger(from) || from < 0 || from >= state.samples.length) throw new Error('搜索起点必须是段内的整数样本索引');
  return { channel: Number($('triggerChannel').value), edge: $('triggerEdge').value, from: Math.max(1, from) };
}

function freezeTrigger(index) {
  state.trigger = index;
  state.cursors.A = index;
  state.view = centeredWindow(index, span());
  $('triggerResult').textContent = `已触发 · D${$('triggerChannel').value} · 索引 ${numberText(index)} · ${sampleTime(index)}`;
  notice('已在原始样本中找到触发沿并冻结显示；触发点位于窗口中部，当前段保留。');
  stopDisplay('triggered', '已触发 · 显示冻结');
}

function single() {
  if (!state.samples) return;
  stopDisplay();
  try {
    const { channel, edge, from } = triggerOptions();
    // 触发始终遍历原始uint16，不使用绘图桶、概览或屏幕像素。
    const index = findEdge(state.samples, channel, edge, from);
    state.available = state.samples.length;
    if (index < 0) {
      state.trigger = -1; $('triggerResult').textContent = '未触发 · 整段搜索完成，当前数据保留';
      stopDisplay('untriggered', '未触发 · 已停止');
    } else {
      state.position = Math.min(state.samples.length, Math.ceil(index + span() / 2));
      freezeTrigger(index);
    }
  } catch (error) { notice(error.message, true); }
}

function run() {
  if (state.metadata?.deviceConnected) { runDevice(); return; }
  if (!state.samples || state.running || state.position >= state.samples.length) return;
  const armed = $('triggerOnRun').checked;
  let options;
  try { options = armed ? triggerOptions() : { channel: 0, edge: 'either', from: 1 }; } catch (error) { notice(error.message, true); return; }
  state.running = true; state.trigger = -1; state.pendingTrigger = -1;
  state.scanFrom = Math.max(1, state.position, options.from);
  const initial = state.position, startedAt = performance.now();
  const rate = state.metadata.samplePeriodNs === null ? 100_000 : 1e9 / state.metadata.samplePeriodNs;
  const windowSpan = span();
  setStatus('running', armed ? '回放中 · 等待触发' : '本机回放中');
  $('triggerResult').textContent = armed ? '等待原始样本中的边沿' : '自由回放 · 触发未启用';
  if (state.metadata.samplePeriodNs === null) notice('采样周期 unknown；仅按 100,000 点/秒演示回放进度，时间轴使用样本索引。', true);
  function frame(now) {
    if (!state.running) return;
    state.position = Math.min(state.samples.length, Math.max(1, initial + Math.floor((now - startedAt) * rate / 1000)));
    state.available = state.position;
    if (armed && state.pendingTrigger < 0) {
      state.pendingTrigger = findEdge(state.samples, options.channel, options.edge, state.scanFrom, state.available);
      state.scanFrom = Math.max(state.scanFrom, state.available);
    }
    if (state.pendingTrigger >= 0 && (state.available >= state.pendingTrigger + windowSpan / 2 || state.available === state.samples.length)) {
      freezeTrigger(state.pendingTrigger); return;
    }
    state.view = { start: Math.max(0, state.position - windowSpan), end: Math.max(windowSpan, state.position) };
    if (state.position >= state.samples.length) {
      if (armed) {
        $('triggerResult').textContent = '未触发 · 已到满深度，当前段保留';
        stopDisplay('untriggered', '未触发 · 已停止');
      } else stopDisplay('full', '满深度 · 已停止');
      notice('本段回放已到满深度，全部原始样本保留；不循环覆盖。需要重新播放时，明确点击“从头回放”。', true);
      return;
    }
    scheduleDraw(); animation = requestAnimationFrame(frame);
  }
  animation = requestAnimationFrame(frame);
}

function runDevice() {
  if (state.running || !state.metadata?.deviceConnected) return;
  const generation = sourceGeneration, display = ++displayGeneration;
  const key = `${state.metadata.sourceEpoch}/${state.metadata.captureId}`;
  const armed = $('triggerOnRun').checked;
  let options;
  try { options = state.samples.length ? triggerOptions() : { channel: Number($('triggerChannel').value), edge: $('triggerEdge').value, from: 1 }; }
  catch (error) { notice(error.message, true); return; }
  state.scanFrom = Math.max(1, state.position, options.from); state.pendingTrigger = -1;
  state.running = true; state.trigger = -1;
  const windowSpan = Math.min(span(), 8192);
  setStatus('running', armed ? '装置显示中 · 等待触发' : '装置显示刷新中');
  async function poll() {
    try {
      const { capture, info } = await serviceCapture();
      if (!state.running || display !== displayGeneration || generation !== sourceGeneration) return;
      if (`${info.sourceEpoch}/${info.captureId}` !== key) throw new Error('装置已换段；当前显示保留，请明确重新载入服务数据');
      if (capture.samples.length < state.samples.length) throw new Error('装置提交点数倒退，当前段保留');
      state.samples = capture.samples; state.metadata = { ...capture.metadata, ...info };
      state.available = state.position = state.samples.length; overviewCache = null;
      updateMetrics();
      if (armed && state.pendingTrigger < 0) {
        state.pendingTrigger = findEdge(state.samples, options.channel, options.edge, state.scanFrom);
        state.scanFrom = Math.max(state.scanFrom, state.available);
      }
      if (state.pendingTrigger >= 0 && (state.available >= state.pendingTrigger + windowSpan / 2 || info.captureState !== 'running')) {
        state.view = centeredWindow(state.pendingTrigger, windowSpan); freezeTrigger(state.pendingTrigger); return;
      }
      state.view = { start: Math.max(0, state.available - windowSpan), end: Math.max(windowSpan, state.available) };
      if (['full', 'fault', 'stopped', 'off'].includes(info.captureState)) {
        if (armed) $('triggerResult').textContent = '未触发 · 当前装置段搜索完成';
        stopDisplay(info.captureState === 'fault' ? 'error' : armed ? 'untriggered' : 'full',
          info.captureState === 'fault' ? '采集故障 · 原段保留' : armed ? '未触发 · 已停止' : '装置段完成 · 显示停止');
        notice(`装置 ${info.captureState}，已保留 ${numberText(info.sampleCount)} 点；丢样 ${numberText(info.droppedSamples)}，错误码 ${info.errorCode}。不循环覆盖。`, info.captureState === 'fault');
        return;
      }
      scheduleDraw(); deviceTimer = setTimeout(poll, DEVICE_REFRESH_MS);
    } catch (error) {
      if (display !== displayGeneration || generation !== sourceGeneration) return;
      stopDisplay('error', '连接异常 · 保留原段'); notice(error.message, true);
    }
  }
  poll();
}

async function armDevice() {
  const generation = sourceGeneration, before = `${state.metadata.sourceEpoch}/${state.metadata.captureId}`;
  const period = Number($('devicePeriod').value), count = Number($('deviceDepth').value), pattern = Number($('devicePattern').value);
  if (!Number.isInteger(count) || count < 1 || count > Math.min(MAX_SAMPLES, Math.floor(CLOCK_HZ / period))) {
    notice('点数必须在当前周期对应的 1 秒 / 1,000,000 点上限内。', true); return;
  }
  $('armDevice').disabled = true;
  stopDisplay();
  let sent = false;
  try {
    const response = await fetch(`/api/device/arm?${new URLSearchParams({ period, count, pattern })}`, {
      method: 'POST', headers: { 'X-Logic16-Arm': '1' }, signal: AbortSignal.timeout(5000),
    });
    sent = true;
    if (!response.ok) throw new Error((await response.json().catch(() => null))?.error ?? '采集命令未获接受');
    const deadline = performance.now() + 5000;
    while (performance.now() < deadline) {
      const { capture, info } = await serviceCapture();
      if (generation !== sourceGeneration) return;
      if (`${info.sourceEpoch}/${info.captureId}` !== before) {
        installCapture(capture, info); runDevice(); return;
      }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error('采集命令结果尚未确认，请先重新载入状态');
  } catch (error) {
    if (generation === sourceGeneration) notice(`${error.message}${sent ? '；本页没有重复发送命令。' : ''} 当前显示段仍保留。`, true);
  } finally { $('armDevice').disabled = false; }
}

function prepareCanvas(canvas, height) {
  const width = Math.max(1, canvas.getBoundingClientRect().width), ratio = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio);
  }
  const ctx = canvas.getContext('2d'); ctx.setTransform(ratio, 0, 0, ratio, 0, 0); ctx.clearRect(0, 0, width, height);
  return { ctx, width, height };
}

function drawPlot() {
  if (!state.samples) return;
  const { ctx, width, height } = prepareCanvas($('plot'), 490);
  const css = getComputedStyle(document.documentElement), grid = css.getPropertyValue('--grid'), muted = css.getPropertyValue('--muted');
  const { start, end } = state.view, count = state.available;
  const x = index => (index - start) / (end - start) * width;
  ctx.font = '10px Consolas, monospace'; ctx.textBaseline = 'middle';
  for (let channel = 0; channel < 16; channel++) {
    const top = 36 + channel * 28;
    if (channel % 2) { ctx.fillStyle = css.getPropertyValue('--row'); ctx.fillRect(0, top, width, 28); }
    ctx.strokeStyle = grid; ctx.lineWidth = .6; ctx.beginPath(); ctx.moveTo(0, top + 28); ctx.lineTo(width, top + 28); ctx.stroke();
  }
  const tickCount = Math.max(2, Math.floor(width / 120));
  for (let i = 0; i <= tickCount; i++) {
    const px = i / tickCount * width, sample = start + i / tickCount * (end - start);
    ctx.strokeStyle = grid; ctx.lineWidth = .6; ctx.beginPath(); ctx.moveTo(px, 29); ctx.lineTo(px, height); ctx.stroke();
    ctx.fillStyle = muted; ctx.textAlign = i === 0 ? 'left' : i === tickCount ? 'right' : 'center';
    ctx.fillText(sampleTime(sample), px + (i === 0 ? 5 : i === tickCount ? -5 : 0), 17);
  }
  const buckets = overviewBuckets(state.samples.subarray(0, count), start, end, width);
  const dense = buckets.some(bucket => bucket.end - bucket.start > 1);
  $('renderMode').textContent = dense ? '桶内高 / 低包络 · 放大查看阶梯' : '逐样本阶梯 · 无插值';
  for (let channel = 0; channel < 16; channel++) {
    const mask = 1 << channel, high = 43 + channel * 28, low = high + 15;
    ctx.strokeStyle = laneColor(channel); ctx.fillStyle = laneColor(channel); ctx.lineWidth = 1.3; ctx.beginPath();
    let previousY = null;
    for (const b of buckets) {
      const left = Math.max(0, x(b.start)), right = Math.min(width, x(b.end));
      const firstY = b.first & mask ? high : low, lastY = b.last & mask ? high : low;
      if (previousY === null) ctx.moveTo(left, firstY);
      else { ctx.lineTo(left, previousY); ctx.lineTo(left, firstY); }
      if ((b.any ^ b.all) & mask) {
        // 密集跳变只画垂直包络，不把抽点结果连成不存在的斜线。
        ctx.lineTo(left, high); ctx.lineTo(left, low); ctx.lineTo(left, lastY);
        ctx.globalAlpha = .16; ctx.fillRect(left, high, Math.max(1, right - left), low - high); ctx.globalAlpha = 1;
      }
      ctx.lineTo(right, lastY); previousY = lastY;
    }
    ctx.stroke();
  }
  if (start < 0 || end > count) {
    ctx.fillStyle = muted; ctx.textAlign = 'center';
    if (start < 0) ctx.fillText('无样本', Math.min(width / 2, x(0) / 2), height - 6);
    if (end > count) ctx.fillText('无样本', Math.max(width / 2, (x(count) + width) / 2), height - 6);
  }
  const markers = [['A', state.cursors.A, css.getPropertyValue('--cursor')], ['B', state.cursors.B, '#82aaff']];
  if (state.trigger >= 0) markers.unshift(['T', state.trigger, css.getPropertyValue('--accent')]);
  for (const [name, index, color] of markers) {
    if (index === null || index < start || index > end) continue;
    const px = x(index); ctx.strokeStyle = color; ctx.fillStyle = color;
    ctx.setLineDash(name === 'T' ? [4, 4] : []); ctx.beginPath(); ctx.moveTo(px, 29); ctx.lineTo(px, height); ctx.stroke(); ctx.setLineDash([]);
    ctx.textAlign = 'left'; ctx.fillText(name, Math.min(width - 10, px + 4), name === 'T' ? 8 : 28);
  }
  const live = count ? state.samples[Math.min(count - 1, state.samples.length - 1)] : null;
  $('liveWord').textContent = live === null ? '—' : wordText(live);
  $('liveIndex').textContent = count ? `显示至索引 ${numberText(count - 1)}` : '尚无样本';
  for (let i = 0; i < 16; i++) $(`level${i}`).textContent = live === null ? '?' : (live >> i) & 1;
  $('viewExtent').textContent = `${sampleTime(start)} → ${sampleTime(end)}`;
  $('pan').value = Math.round(Math.max(0, Math.min(1, (start + (end - start) / 2) / Math.max(1, state.samples.length))) * 10000);
  updateCursors(); drawOverview();
}

function drawOverview() {
  const { ctx, width, height } = prepareCanvas($('overview'), 58);
  if (!overviewCache || overviewCache.width !== width) overviewCache = { width, buckets: overviewBuckets(state.samples, 0, state.samples.length, width) };
  const x = index => index / Math.max(1, state.samples.length) * width;
  ctx.globalAlpha = .65;
  for (let channel = 0; channel < 16; channel++) {
    ctx.fillStyle = laneColor(channel);
    for (const b of overviewCache.buckets) {
      const mask = 1 << channel;
      if (b.any & mask) ctx.fillRect(x(b.start), channel * 3 + 3, Math.max(.5, x(b.end - b.start)), (b.all & mask) ? 1 : 2);
    }
  }
  ctx.globalAlpha = 1;
  const left = Math.max(0, x(state.view.start)), right = Math.min(width, x(state.view.end));
  ctx.fillStyle = 'rgba(85,222,193,.13)'; ctx.fillRect(left, 0, Math.max(2, right - left), height);
  ctx.strokeStyle = '#55dec1'; ctx.strokeRect(left, .5, Math.max(2, right - left), height - 1);
}

function scheduleDraw() {
  if (drawQueued) return;
  drawQueued = true; requestAnimationFrame(() => { drawQueued = false; drawPlot(); });
}

function zoom(factor, anchor = .5) {
  const width = span(), next = Math.max(2, Math.min(state.samples.length * 2, width * factor));
  const fixed = state.view.start + width * anchor;
  setView(fixed - next * anchor, fixed + next * (1 - anchor));
}

function selectTool(tool) {
  state.tool = tool;
  for (const [id, value] of [['cursorA', 'A'], ['cursorB', 'B'], ['panTool', 'pan']]) {
    $(id).classList.toggle('active', tool === value); $(id).setAttribute('aria-pressed', tool === value);
  }
  $('plot').style.cursor = tool === 'pan' ? 'grab' : 'crosshair';
}

for (let channel = 0; channel < 16; channel++) {
  const option = document.createElement('option'); option.value = channel; option.textContent = `D${channel}`; $('triggerChannel').append(option);
  const label = document.createElement('div'); label.className = 'lane-label'; label.style.setProperty('--lane', colors[channel]);
  label.innerHTML = `<span class="lane-dot"></span><b>D${channel}</b><output id="level${channel}">?</output>`;
  $('laneLabels').append(label);
}
for (let channel = 0; channel < MONITOR_CHANNELS; channel++) {
  const indicator = document.createElement('div');
  indicator.className = 'monitor-bit';
  indicator.innerHTML = `<span>D${channel}</span><small>YX${channel + 1}</small><output>?</output>`;
  $('monitorBits').append(indicator);
}
$('monitorToggle').addEventListener('click', () => {
  if (monitoring) {
    stopMonitor();
    $('monitorStatus').textContent = '网页监视已暂停，读数冻结；CPU3 继续读取';
  } else startMonitor();
});
$('run').addEventListener('click', run);
$('stop').addEventListener('click', () => stopDisplay());
$('single').addEventListener('click', single);
$('restart').addEventListener('click', () => {
  state.position = 0; state.available = state.samples.length; state.trigger = -1;
  state.pendingTrigger = -1; $('triggerResult').textContent = '尚未搜索触发';
  notice('已明确回到本段起点；已保留的原始数据不变。点击运行开始本机回放。'); setStatus('ready', '已回到起点 · 显示停止'); scheduleDraw();
});
$('reloadSource').addEventListener('click', loadService);
$('armDevice').addEventListener('click', armDevice);
$('devicePeriod').addEventListener('change', () => {
  const max = Math.min(MAX_SAMPLES, Math.floor(CLOCK_HZ / Number($('devicePeriod').value)));
  $('deviceDepth').max = max; $('deviceDepth').value = Math.min(max, Number($('deviceDepth').value));
});
$('loadFile').addEventListener('click', loadLocalFile);
$('fit').addEventListener('click', () => state.samples && setView(0, state.samples.length));
$('showNotch').addEventListener('click', () => { stopDisplay(); state.available = state.samples.length; setView(500208, 500320); });
$('zoomIn').addEventListener('click', () => state.samples && zoom(.5));
$('zoomOut').addEventListener('click', () => state.samples && zoom(2));
$('panLeft').addEventListener('click', () => state.samples && setView(state.view.start - span() / 4, state.view.end - span() / 4));
$('panRight').addEventListener('click', () => state.samples && setView(state.view.start + span() / 4, state.view.end + span() / 4));
$('pan').addEventListener('input', () => { if (state.samples) { const center = Number($('pan').value) / 10000 * state.samples.length; setView(center - span() / 2, center + span() / 2); } });
for (const [id, tool] of [['cursorA', 'A'], ['cursorB', 'B'], ['panTool', 'pan']]) $(id).addEventListener('click', () => selectTool(tool));
$('locate').addEventListener('click', () => {
  if (!state.samples) return;
  const index = Number($('cursorIndex').value);
  if (!Number.isSafeInteger(index) || index < 0 || index >= state.available) { notice('游标索引必须在当前已有原始样本内。', true); return; }
  state.cursors[state.tool === 'B' ? 'B' : 'A'] = index;
  if (index < state.view.start || index >= state.view.end) state.view = centeredWindow(index, span());
  scheduleDraw();
});
$('plot').addEventListener('wheel', event => {
  if (!state.samples) return;
  event.preventDefault(); const rect = $('plot').getBoundingClientRect();
  zoom(event.deltaY > 0 ? 1.5 : 1 / 1.5, Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)));
}, { passive: false });
$('plot').addEventListener('pointerdown', event => {
  if (!state.samples) return;
  const rect = $('plot').getBoundingClientRect();
  if (state.tool === 'pan') { drag = { x: event.clientX, start: state.view.start, width: span(), pixels: rect.width }; $('plot').setPointerCapture(event.pointerId); }
  else {
    const index = Math.floor(state.view.start + (event.clientX - rect.left) / rect.width * span());
    if (index >= 0 && index < state.available) { state.cursors[state.tool] = index; $('cursorIndex').value = index; scheduleDraw(); }
  }
});
$('plot').addEventListener('pointermove', event => { if (drag) { const start = drag.start - (event.clientX - drag.x) / drag.pixels * drag.width; setView(start, start + drag.width); } });
for (const event of ['pointerup', 'pointercancel', 'lostpointercapture']) $('plot').addEventListener(event, () => { drag = null; });
$('plot').addEventListener('keydown', event => {
  if (!state.samples) return;
  if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') { const delta = span() / 4 * (event.key === 'ArrowLeft' ? -1 : 1); setView(state.view.start + delta, state.view.end + delta); }
  else if (event.key === '+' || event.key === '=') zoom(.5);
  else if (event.key === '-') zoom(2);
  else return;
  event.preventDefault();
});
$('overview').addEventListener('pointerdown', event => { if (state.samples) { const rect = $('overview').getBoundingClientRect(), center = (event.clientX - rect.left) / rect.width * state.samples.length; setView(center - span() / 2, center + span() / 2); } });
$('theme').addEventListener('click', () => {
  document.documentElement.dataset.theme = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  for (let channel = 0; channel < 16; channel++) $('laneLabels').children[channel].style.setProperty('--lane', laneColor(channel));
  scheduleDraw();
});
new ResizeObserver(scheduleDraw).observe($('plot'));
if (window.matchMedia('(max-width: 760px)').matches) $('settingsPanel').open = false;
window.lucide?.createIcons();
loadService();
