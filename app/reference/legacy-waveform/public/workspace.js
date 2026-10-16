/* global WorkspaceModel, uPlot, rtSpectrum */
(() => {
  'use strict';
  const M = WorkspaceModel, $ = id => document.getElementById(id);
  const fmt = (v, digits = 3) => Number.isFinite(v) ? v.toLocaleString('zh-CN', { maximumFractionDigits: digits }) : '—';
  const node = (tag, text, className) => { const el = document.createElement(tag); if (text !== undefined) el.textContent = text; if (className) el.className = className; return el; };
  const RENDER_INTERVAL_MS = 200;
  let snapshot = null, socket = null, reconnect = null, closed = false, latestAt = 0, scheduled = false, renderTimer = null, lastRender = 0;
  let page = 'wave', layout = 'split', physical = true, charts = [], chartKey = '', lastChartSource = null, updating = false;
  let view = null, hidden = new Set(), cursorTarget = null, cursors = [null, null], hovered = null;
  let worker = null, workerId = 0, workerTimer = null, detail = null, rawLoaded = false, loading = false, rawReport = null;
  let legendKey = '', triggerInitialized = false, pendingSelect = null, pendingWindow = null, noticeTimer = null, lastMetaKey = null;
  const perf = { messages: 0, rendered: 0, invalidMessages: 0, coalesced: 0, renderMs: [], delayMs: [], longTasks: 0, longTaskMs: 0, startedAt: new Date().toISOString() };
  try { new PerformanceObserver(list => { for (const e of list.getEntries()) { perf.longTasks++; perf.longTaskMs += e.duration; } }).observe({ type: 'longtask', buffered: true }); } catch (_) { /* 旧浏览器不支持时保留未知，不影响绘图。 */ }
  const push = (list, value) => { list.push(value); if (list.length > 600) list.shift(); };
  const summary = values => { const a = [...values].sort((x, y) => x - y); return { count: a.length, p50: a[Math.floor(a.length * .5)] ?? null, p95: a[Math.floor(a.length * .95)] ?? null, max: a.at(-1) ?? null }; };
  function notice(text) { clearTimeout(noticeTimer); $('notice').textContent = text; $('notice').hidden = false; noticeTimer = setTimeout(() => $('notice').hidden = true, 6500); }
  function send(command) {
    if (socket?.readyState !== WebSocket.OPEN) { notice('本机网页服务未连接，请待重连后操作'); return false; }
    socket.send(JSON.stringify(command)); return true;
  }
  for (let i = 1; i <= 16; i++) { const o = node('option', `第 ${String(i).padStart(2, '0')} 开关`); o.value = i; $('switchSelect').append(o); }
  function selectCircuit() {
    if (!snapshot || !$('switchSelect').value) return;
    const wanted = M.selection(snapshot.channels, Number($('switchSelect').value), Number($('voltageSelect').value));
    if (send({ type: 'select', selected: wanted, windowMs: pendingWindow ?? snapshot.windowMs })) { pendingSelect = wanted; hidden.clear(); clearDetail(); resetView(); }
  }
  $('switchSelect').addEventListener('change', selectCircuit); $('voltageSelect').addEventListener('change', selectCircuit);
  $('windowSelect').addEventListener('change', () => {
    const wanted = Number($('windowSelect').value);
    if (snapshot && send({ type: 'select', selected: pendingSelect || snapshot.selected, windowMs: wanted })) { pendingWindow = wanted; clearDetail(); resetView(); }
  });
  function refreshChannels() {
    const key = JSON.stringify([snapshot.channels, snapshot.selected]);
    if (legendKey === key) return;
    legendKey = key; hidden = new Set([...hidden].filter(i => snapshot.selected.includes(i)));
    const inferred = M.inferSelection(snapshot.channels, snapshot.selected);
    if (!pendingSelect) { $('switchSelect').value = inferred?.circuit ?? ''; if (inferred) $('voltageSelect').value = inferred.voltage; }
    const metric = Number($('metricChannel').value);
    $('metricChannel').replaceChildren(...snapshot.selected.map(i => { const o = node('option', snapshot.channels[i].name); o.value = i; return o; }));
    $('metricChannel').value = snapshot.selected.includes(metric) ? metric : snapshot.selected[0];
    $('triggerChannel').replaceChildren(...snapshot.channels.map(ch => { const o = node('option', ch.name); o.value = ch.index; return o; }));
    if (triggerInitialized) $('triggerChannel').value = snapshot.config.channel;
    refreshLegend(); refreshDirectory();
  }
  function refreshLegend() {
    $('channelLegend').replaceChildren(...snapshot.selected.map((index, i) => {
      const ch = snapshot.channels[index], row = node('div', undefined, `channel-row${hidden.has(index) ? ' muted' : ''}`);
      const swatch = node('i', undefined, 'swatch'); swatch.style.background = M.COLORS[i];
      const name = node('span', ch.name, 'channel-name'); row.title = `${ch.physical} · ${ch.calibration} · ${ch.unit}`;
      const button = node('button', hidden.has(index) ? '显示' : '隐藏'); button.setAttribute('aria-label', `${hidden.has(index) ? '显示' : '隐藏'}${ch.name}`);
      button.onclick = () => {
        if (!hidden.has(index) && hidden.size === snapshot.selected.length - 1) return notice('至少保留一条可见波形');
        if (hidden.has(index)) hidden.delete(index); else hidden.add(index);
        chartKey = ''; refreshLegend(); requestRender();
      };
      row.append(swatch, name, button); return row;
    }));
  }
  function refreshDirectory() {
    if (!snapshot) return;
    const query = $('channelSearch').value.trim().toLowerCase();
    $('channelDirectory').replaceChildren(...snapshot.channels.filter(ch => `${ch.name} ${ch.physical}`.toLowerCase().includes(query)).map(ch => {
      const p = node('p', ch.name + ' '); p.append(node('span', ch.physical)); return p;
    }));
  }
  $('channelSearch').addEventListener('input', refreshDirectory);
  function setPage(next) {
    page = next; chartKey = ''; destroyCharts();
    document.querySelectorAll('[data-page]').forEach(el => { if (el.dataset.page === next) el.setAttribute('aria-current', 'page'); else el.removeAttribute('aria-current'); });
    document.querySelectorAll('.page-panel').forEach(el => el.hidden = el.id !== 'page-' + next);
    requestRender();
  }
  document.querySelectorAll('[data-page]').forEach(el => el.addEventListener('click', () => setPage(el.dataset.page)));
  $('showEvents').onclick = () => setPage('record');
  function setLayout(next) { layout = next; chartKey = ''; $('layoutSplit').setAttribute('aria-pressed', String(next === 'split')); $('layoutOverlay').setAttribute('aria-pressed', String(next === 'overlay')); requestRender(); }
  $('layoutSplit').onclick = () => setLayout('split'); $('layoutOverlay').onclick = () => setLayout('overlay');
  function setUnits(value) { physical = value; chartKey = ''; $('unitPhysical').setAttribute('aria-pressed', String(value)); $('unitRaw').setAttribute('aria-pressed', String(!value)); requestRender(); }
  $('unitPhysical').onclick = () => setUnits(true); $('unitRaw').onclick = () => setUnits(false);
  function destroyCharts() { charts.forEach(c => c.destroy()); charts = []; lastChartSource = null; $('plots').replaceChildren(); $('fftPlots').replaceChildren(); }
  function resetView() { view = null; cursors = [null, null]; hovered = null; if (rawLoaded) requestRawView(); chartKey = ''; requestRender(); }
  $('resetView').onclick = resetView;
  function applyView(next, requestDetail = true) {
    if (!next || !Number.isFinite(next.min) || !Number.isFinite(next.max) || next.max <= next.min) return;
    const source = detail?.data || snapshot?.data, start = detail ? 0 : source?.[0]?.[0], end = detail?.durationMs ?? source?.[0]?.at(-1);
    if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) return;
    const width = Math.min(end - start, Math.max(.1, next.max - next.min));
    const min = Math.max(start, Math.min(next.min, end - width)); view = { min, max: min + width };
    updating = true; try { charts.forEach(c => c.setScale('x', view)); } finally { updating = false; }
    if (requestDetail) {
      if (rawLoaded) requestRawView();
      else if (snapshot.displayDecimated && !loading) loadRaw();
    }
  }
  function zoom(factor, center) {
    if (!charts.length || page !== 'wave') return;
    const scale = charts[0].scales.x, middle = center ?? (scale.min + scale.max) / 2;
    applyView({ min: middle - (middle - scale.min) * factor, max: middle + (scale.max - middle) * factor });
  }
  $('zoomIn').onclick = () => zoom(.5); $('zoomOut').onclick = () => zoom(2);
  function controls(chart) {
    const over = chart.over; let pan = null, dragStart = null;
    const wheel = e => { if (page !== 'wave') return; e.preventDefault(); zoom(e.deltaY < 0 ? .8 : 1.25, chart.posToVal(e.clientX - over.getBoundingClientRect().left, 'x')); };
    const down = e => {
      if (e.button === 0 && cursorTarget !== null && page === 'wave') {
        e.preventDefault(); e.stopImmediatePropagation(); cursors[cursorTarget] = chart.posToVal(e.clientX - over.getBoundingClientRect().left, 'x');
        cursorTarget = null; updateCursorButtons(); charts.forEach(c => c.redraw()); cursorReadout();
      } else if ((e.shiftKey || e.button === 1) && page === 'wave') {
        e.preventDefault(); e.stopImmediatePropagation(); pan = { x: e.clientX, min: chart.scales.x.min, max: chart.scales.x.max };
      } else if (e.button === 0 && page === 'wave') dragStart = e.clientX;
    };
    const move = e => { if (pan) { const delta = (pan.x - e.clientX) / over.clientWidth * (pan.max - pan.min); applyView({ min: pan.min + delta, max: pan.max + delta }, false); } };
    const up = e => {
      if (pan) { pan = null; if (rawLoaded) requestRawView(); }
      if (dragStart !== null && Math.abs(e.clientX - dragStart) > 5) {
        const left = over.getBoundingClientRect().left;
        const a = chart.posToVal(dragStart - left, 'x'), b = chart.posToVal(e.clientX - left, 'x');
        applyView({ min: Math.min(a, b), max: Math.max(a, b) });
        chart.setSelect({ left: 0, top: 0, width: 0, height: 0 }, false);
      }
      dragStart = null;
    };
    const reset = () => resetView();
    over.addEventListener('wheel', wheel, { passive: false }); over.addEventListener('mousedown', down, true); over.addEventListener('dblclick', reset);
    window.addEventListener('mousemove', move); window.addEventListener('mouseup', up);
    (chart.hooks.destroy ||= []).push(() => { over.removeEventListener('wheel', wheel); over.removeEventListener('mousedown', down, true); over.removeEventListener('dblclick', reset); window.removeEventListener('mousemove', move); window.removeEventListener('mouseup', up); });
  }
  function cursorDrawing(chart) {
    if (page !== 'wave') return;
    const ctx = chart.ctx; ctx.save(); ctx.beginPath(); ctx.rect(chart.bbox.left, chart.bbox.top, chart.bbox.width, chart.bbox.height); ctx.clip();
    cursors.forEach((value, i) => { if (value === null) return; const x = chart.valToPos(value, 'x', true); ctx.strokeStyle = i ? '#8765bb' : '#687787'; ctx.setLineDash([5, 4]); ctx.beginPath(); ctx.moveTo(x, chart.bbox.top); ctx.lineTo(x, chart.bbox.top + chart.bbox.height); ctx.stroke(); }); ctx.restore();
  }
  function cursorReadout() {
    if (!snapshot) return;
    const data = detail?.data || snapshot.data, chIndex = Number($('metricChannel').value), pos = snapshot.selected.indexOf(chIndex), ch = snapshot.channels[chIndex];
    const unit = physical ? ch.unit : 'code', parts = [];
    cursors.forEach((x, i) => {
      if (x === null) return;
      const j = M.lowerBound(data[0], x);
      const value = j < data[0].length ? data[pos + 1]?.[j] : null;
      parts.push(`${i ? 'B' : 'A'} ${fmt(x)} ms / ${ch.name} ${fmt(value === null ? null : physical ? value * ch.a + ch.b : value)} ${unit}`);
    });
    if (cursors.every(x => x !== null)) parts.push(`Δt ${fmt(cursors[1] - cursors[0])} ms`);
    if (hovered !== null && !parts.length) parts.push(`同步光标 ${fmt(hovered)} ms`);
    $('cursorReadout').textContent = parts.join(' · ') || '框选或滚轮同步缩放 · Shift拖动平移 · 双击复位';
  }
  function updateCursorButtons() { $('cursorA').setAttribute('aria-pressed', String(cursorTarget === 0)); $('cursorB').setAttribute('aria-pressed', String(cursorTarget === 1)); }
  $('cursorA').onclick = () => { cursorTarget = cursorTarget === 0 ? null : 0; updateCursorButtons(); };
  $('cursorB').onclick = () => { cursorTarget = cursorTarget === 1 ? null : 1; updateCursorButtons(); };
  function renderCharts() {
    if (!snapshot || !['wave', 'fft'].includes(page)) return;
    const visible = snapshot.selected.filter(i => !hidden.has(i)), isFft = page === 'fft';
    let source = isFft ? snapshot.fftData : detail?.data || snapshot.data;
    if (!source?.[0]?.length) { destroyCharts(); chartKey = ''; if (isFft) $('fftState').textContent = '没有连续原始尾窗，频谱不可计算'; return; }
    const channels = visible.map(i => snapshot.channels[i]);
    const series = [source[0], ...visible.map(i => source[snapshot.selected.indexOf(i) + 1])];
    let data = M.chartData(series, channels, physical);
    if (isFft) {
      const spectra = data.slice(1).map(y => rtSpectrum(data[0], y));
      if (spectra.some(s => !s)) { destroyCharts(); chartKey = ''; $('fftState').textContent = '连续时标或有效样点不足，频谱不可计算'; return; }
      data = [spectra[0][0], ...spectra.map(s => s[1])];
      $('fftState').textContent = `${snapshot.fftData[0].length} 个原始尾窗点作为输入；FFT函数至多取2048点 · 峰值谱 / ${physical ? '通道单位' : 'ADC码'}`;
    }
    const groups = layout === 'overlay' ? [channels.map((_, i) => i)] : channels.map((_, i) => [i]);
    const key = JSON.stringify([page, layout, physical, channels]);
    const sourceKey = detail ? source : snapshot.frozen ? `${snapshot.firstSampleTime}:${snapshot.lastSampleTime}:${snapshot.sourceSampleCount}` : source;
    if (key === chartKey && sourceKey === lastChartSource) return;
    updating = true;
    try {
      if (chartKey !== key) {
        destroyCharts(); chartKey = key;
        const host = isFft ? $('fftPlots') : $('plots');
        for (const indices of groups) {
          const row = node('div', undefined, 'plot-row'), title = node('p', indices.map(i => channels[i].name).join(' / '), 'plot-label');
          title.style.color = M.COLORS[snapshot.selected.indexOf(channels[indices[0]].index)]; row.append(title); host.append(row);
          const units = [...new Set(indices.map(i => physical ? channels[i].unit : 'code'))], scales = { x: { time: false } };
          units.forEach(unit => { scales[unit] = { range: (_, min, max) => { const peak = Math.max(Math.abs(min ?? 0), Math.abs(max ?? 0)) || 1; return isFft ? [0, peak * 1.12] : [-peak * 1.12, peak * 1.12]; } }; });
          const chart = new uPlot({ width: Math.max(200, host.clientWidth), height: groups.length === 1 ? 430 : 122,
            padding: [7, 14, 0, 0], scales, legend: { show: false },
            cursor: { drag: { x: true, y: false, setScale: false }, sync: { key: `t153-workspace-${page}`, scales: ['x', null], setSeries: false } },
            series: [{ label: isFft ? 'Hz' : 'ms' }, ...indices.map(i => ({ label: channels[i].name, scale: physical ? channels[i].unit : 'code',
              stroke: M.COLORS[snapshot.selected.indexOf(channels[i].index)], width: 1.3, spanGaps: false, points: { show: false } }))],
            axes: [{ stroke: '#8a9198', grid: { stroke: '#f0f1f3' }, size: 28, font: '11px system-ui', values: (_, ticks) => ticks.map(v => fmt(v, 1)) },
              ...units.map((unit, i) => ({ scale: unit, side: i ? 1 : 3, size: 56, space: 30, stroke: '#8a9198', grid: { show: i === 0, stroke: '#eceef0' }, font: '11px system-ui', values: (_, ticks) => ticks.map(v => fmt(v, 2) + (unit === 'code' ? '' : ` ${unit}`)) }))],
            hooks: { ready: [controls], draw: [cursorDrawing],
              setCursor: [c => { if (page === 'wave' && c.cursor.left >= 0) { hovered = c.posToVal(c.cursor.left, 'x'); cursorReadout(); } }] }
          }, [data[0], ...indices.map(i => data[i + 1])], row);
          charts.push(chart);
        }
      } else charts.forEach((chart, j) => chart.setData([data[0], ...groups[j].map(i => data[i + 1])], !view || isFft));
      if (view && !isFft) charts.forEach(chart => chart.setScale('x', view));
      lastChartSource = sourceKey;
    } finally { updating = false; }
    cursorReadout();
    if (!isFft) {
      const count = detail?.sourceCount ?? snapshot.sourceSampleCount, displayed = source[0].length;
      $('dataExtent').textContent = `${detail ? '一次性原始历史' : '当前窗口'} · ${fmt(count, 0)} 原始点 / ${fmt(displayed, 0)} 绘图点 · ${detail?.decimated ?? snapshot.displayDecimated ? '峰值抽点，继续放大可看原始点' : '逐原始点显示'} · ${physical ? '工程量 A / V' : 'ADC有符号码'}`;
    }
  }
  const resize = new ResizeObserver(() => {
    const host = page === 'fft' ? $('fftPlots') : $('plots');
    if (!host.clientWidth) return;
    updating = true; try { charts.forEach(c => c.setSize({ width: Math.max(200, host.clientWidth), height: c.height })); } finally { updating = false; }
  }); resize.observe($('content-panel') || document.querySelector('.content-panel'));
  function requestRawView() {
    if (!rawLoaded || !snapshot) return;
    clearTimeout(workerTimer); workerTimer = setTimeout(() => worker.postMessage({ type: 'view', id: ++workerId, selected: snapshot.selected, windowMs: snapshot.windowMs, range: view }), 90);
  }
  function clearDetail() {
    workerId++; clearTimeout(workerTimer); worker?.terminate(); worker = null; detail = null; rawLoaded = false; loading = false; rawReport = null;
    $('rawDetail').disabled = false; $('liveView').hidden = true; $('detailState').textContent = ''; chartKey = '';
  }
  function loadRaw() {
    if (!snapshot || loading) return;
    loading = true; rawLoaded = false; $('rawDetail').disabled = true; $('detailState').textContent = '正在校验完整80路原始历史；此操作只在本次查看时读取，不影响装置采集。';
    worker ||= new Worker('/workspace/worker.js');
    worker.onmessage = ({ data }) => {
      if (data.id !== workerId) return;
      if (data.type === 'error') { loading = false; $('rawDetail').disabled = false; $('detailState').textContent = data.text; notice(data.text); return; }
      if (data.type === 'loaded') {
        if (M.session(data.meta) !== M.session(snapshot.frozen ? snapshot.captureMeta : snapshot.meta)) { clearDetail(); notice('读取期间采集会话已变化，请重新获取原始历史'); return; }
        rawLoaded = true; rawReport = data.report;
      }
      if (data.type === 'view') {
        detail = data; loading = false; $('rawDetail').disabled = false; $('liveView').hidden = false;
        $('detailState').textContent = `本地历史细节 · 获取于 ${data.savedAt} · ${data.sourceFrozen ? '原冻结记录' : '本次获取后保持静止，装置与后台继续采集'} · 80路 / 序号缺口 ${rawReport?.gaps ?? '未知'} / 无效 ${rawReport?.invalid ?? '未知'} / 估算 ${rawReport?.estimated ?? '未知'}`;
        chartKey = ''; requestRender();
      }
    };
    worker.onerror = () => { loading = false; $('rawDetail').disabled = false; $('detailState').textContent = '原始历史Worker失败，请重新加载细节'; };
    worker.postMessage({ type: 'load', id: ++workerId, selected: snapshot.selected, windowMs: snapshot.windowMs, range: view });
  }
  $('rawDetail').onclick = loadRaw; $('liveView').onclick = () => { clearDetail(); resetView(); };
  $('pauseDisplay').onclick = () => send({ type: 'pause' });
  $('resumeDisplay').onclick = () => { if (send({ type: 'resume' })) { clearDetail(); resetView(); } };
  $('exportCapture').onclick = () => send({ type: 'export' });
  function fillTrigger() {
    if (triggerInitialized) return;
    const c = snapshot.config;
    for (const [id, field] of Object.entries({ triggerChannel: 'channel', triggerCondition: 'condition', triggerThreshold: 'threshold', triggerConsecutive: 'consecutive', triggerReference: 'referenceCycles', triggerPre: 'preMs', triggerPostMode: 'postMode', triggerPostCycles: 'postCycles', triggerPostMs: 'postMs' })) $(id).value = c[field];
    triggerInitialized = true;
  }
  $('triggerForm').addEventListener('submit', event => {
    event.preventDefault();
    const config = { channel: Number($('triggerChannel').value), condition: $('triggerCondition').value, threshold: Number($('triggerThreshold').value), consecutive: Number($('triggerConsecutive').value), referenceCycles: Number($('triggerReference').value), preMs: Number($('triggerPre').value), postMode: $('triggerPostMode').value, postCycles: Number($('triggerPostCycles').value), postMs: Number($('triggerPostMs').value) };
    if (send({ type: 'arm', config })) { clearDetail(); resetView(); notice('已提交电脑端触发配置，等待状态返回'); }
  });
  function renderMeasurements() {
    const m = snapshot.measurements?.find(item => item.index === Number($('metricChannel').value)), ch = snapshot.channels[Number($('metricChannel').value)];
    $('metricCards').replaceChildren(...[['交流 RMS', m?.acRms, ch.unit], ['频率', m?.frequency, 'Hz'], ['峰峰值', m?.pp, ch.unit]].map(([name, value, unit]) => { const card = node('div', undefined, 'metric-card'); card.append(node('span', name), node('strong', `${fmt(value)} ${unit}`)); return card; }));
    if (page !== 'measure') return;
    $('measurementRows').replaceChildren(...(snapshot.measurements || []).map(m => {
      const ch = snapshot.channels[m.index], tr = node('tr');
      const values = [ch.name, `${fmt(m.acRms, 5)} ${ch.unit}`, `${fmt(m.rms, 5)} ${ch.unit}`, `${fmt(m.fundamentalRms, 5)} ${ch.unit}`, fmt(m.thdPercent) + ' %', fmt(m.frequency) + ' Hz', `${fmt(m.dc)} ${ch.unit}`, `${fmt(m.pp)} ${ch.unit}`, `${fmt(m.count, 0)} / ${m.fft?.reason || m.fft?.thdReason || '有效'}`];
      tr.append(...values.map(v => node('td', v))); return tr;
    }));
  }
  $('metricChannel').onchange = () => { renderMeasurements(); cursorReadout(); };
  function renderEvents() {
    if (page !== 'record') return;
    const filter = $('eventFilter').value;
    const rows = (snapshot.events || []).filter(e => filter === 'all' || e.type === filter || (filter === 'fault' && ['fault', 'disconnect', 'stalled', 'reboot'].includes(e.type)));
    $('eventList').replaceChildren(...rows.map(e => {
      const row = node('div', undefined, 'event-row'), time = node('div', e.sampleTime || e.time || '未知时间'), body = node('div', e.text || e.type);
      time.append(node('small', `${e.sampleTime ? '样点时间' : '电脑观察时间'} · ${e.type}`));
      if (e.sampleTime) body.append(node('small', `电脑观察：${e.time || '未知'} · 样点序号：${e.sequence ?? '未知'}`));
      if (e.type === 'saved' && /^[\w.-]+$/.test(e.text)) for (const ext of ['cfg', 'dat', 'json']) { const a = node('a', ext.toUpperCase()); a.href = '/captures/' + e.text + '.' + ext; body.append(a); }
      if (e.sampleTime) {
        const button = node('button', '定位到波形'); button.onclick = () => {
          if (detail) return notice('当前为独立历史细节，请先返回流视图后定位事件');
          const x = Date.parse(e.sampleTime) - Date.parse(snapshot.firstSampleTime), last = snapshot.data[0].at(-1);
          if (!Number.isFinite(x) || !Number.isFinite(last) || x < 0 || x > last) return notice('该事件已在当前显示窗口之外，请下载对应录波查看');
          setPage('wave'); cursors[0] = x; requestRender();
        }; body.append(button);
      }
      if (e.reason) body.append(node('small', e.reason)); row.append(time, body); return row;
    }));
    if (!rows.length) $('eventList').append(node('p', '当前筛选没有事件', 'hint'));
    $('digital').replaceChildren(...Array.from({ length: 64 }, (_, i) => node('span', `D${i + 1}: ${snapshot.digital?.[i] ?? '?'}`, snapshot.digital?.[i] === 1 ? 'on' : '')));
  }
  $('eventFilter').onchange = renderEvents;
  function dl(id, values) { $(id).replaceChildren(...values.flatMap(([key, value]) => [node('dt', key), node('dd', value === null || value === undefined ? '未知' : String(value))])); }
  function renderDiagnostics() {
    if (page !== 'diagnostics') return;
    const current = M.diagnosticHealth(snapshot), m = snapshot.meta, h = current.fresh ? snapshot.deviceHealth : {}, a = current.fields, s = snapshot.stats;
    dl('diagnosticValues', [['采集归属', h.acquisitionOwner], ['采集核状态 / 故障位', `${m.state ?? '未知'} / ${m.faults === undefined ? '未知' : '0x' + m.faults.toString(16)}`], ['会话 / 运行代次', `${m.epoch ?? '未知'} / ${m.generation ?? '未知'}`], ['实际ADC掩码', m.enabledChipMask === undefined ? null : '0x' + m.enabledChipMask.toString(16)], ['采集错误总数', a.errors ?? h.adc_error_total], ['漏转换周期', a.missed_cycles ?? h.sample_missed], ['总线读取错误', a.read_errors], ['共享环错误', a.ring_errors], ['BUSY超时', a.busy_timeouts], ['采集报告新鲜有效', h.acquisitionFresh === undefined ? null : h.acquisitionFresh ? '是' : '否'], ['健康核对时间', h.checkedAt], ['累计样点 / 缺口', `${fmt(s.frames, 0)} / ${s.gaps ?? '未知'}`], ['窗口发送丢帧', snapshot.streamDrops], ['RTC异常', s.rtcErrors], ['时钟质量', m.timeQuality === 1 ? '软件UTC，未PPS锁定' : m.timeQuality === 2 ? 'PPS锁定' : '未同步 / 未知'], ['统计起点', s.startedAt], ['80路原始历史核验', rawReport ? `${rawReport.frames}帧，缺口${rawReport.gaps}，无效${rawReport.invalid}，估算${rawReport.estimated}，未知质量${rawReport.unknownQuality}` : '尚未按需载入']]);
    const render = summary(perf.renderMs), delay = summary(perf.delayMs);
    dl('performanceValues', [['收到 / 已绘制刷新', `${perf.messages} / ${perf.rendered}`], ['结构无效快照', perf.invalidMessages], ['合并过时显示刷新', perf.coalesced], ['绘图 p50 / p95 / max', `${fmt(render.p50)} / ${fmt(render.p95)} / ${fmt(render.max)} ms`], ['消息到绘图 p95 / max', `${fmt(delay.p95)} / ${fmt(delay.max)} ms`], ['页面长任务', `${perf.longTasks} 次 / ${fmt(perf.longTaskMs)} ms`], ['浏览器JS堆', performance.memory ? `${fmt(performance.memory.usedJSHeapSize / 1048576, 1)} MiB` : '浏览器未提供'], ['本页运行起点', perf.startedAt]]);
  }
  function renderStatus() {
    const online = socket?.readyState === WebSocket.OPEN && Date.now() - latestAt < 3500;
    const fresh = M.streamFresh(snapshot, online);
    $('connection').textContent = !online ? '网页连接中断' : fresh ? '设备采样在线' : ({ fault: '采集故障', stalled: '采样停滞', waiting: '等待样点', connecting: '设备连接中', offline: '设备未连接' })[snapshot.status] || '状态未知';
    $('connection').className = fresh ? 'good' : 'bad';
    const m = snapshot.meta, h = snapshot.deviceHealth || {}, rate = m.tickHz && m.period ? m.tickHz / m.period : null;
    $('sampleRate').textContent = rate ? `${fmt(rate / 1000, 1)} kSPS` : '未知采样率';
    const owner = M.diagnosticHealth(snapshot).owner;
    $('sourceLine').textContent = `T153 · ${owner === 'a7_amp' ? 'A7 独立采集' : owner || '采集归属待核对'} · ${snapshot.deviceHost || '未知地址'} · 80路目录`;
    $('displayState').textContent = detail ? '本地历史细节（非实时）' : snapshot.capturing ? '后触发录波中' : snapshot.frozen ? '服务端已冻结' : !fresh ? '等待新样点（末次窗口）' : snapshot.armed ? '实时显示 · 已布防' : '实时显示';
    $('qualityState').textContent = `源故障 ${m.faults === undefined ? '未知' : '0x' + m.faults.toString(16)} · 本机累计缺口 ${snapshot.stats.gaps ?? '未知'} · ${m.timeQuality === 1 ? '软件UTC' : m.timeQuality === 2 ? 'PPS锁定' : '时钟未同步'}`;
    $('qualityState').className = m.faults || snapshot.stats.gaps ? 'warning' : '';
    $('sampleAge').textContent = snapshot.connection?.lastSample ? `距新样点 ${fmt(Math.max(0, Date.now() - snapshot.connection.lastSample) / 1000, 1)} s` : '样点新鲜度未知';
    const scope = `${snapshot.frozen ? '服务端冻结窗口' : fresh ? '服务端完整窗口' : '服务端末次窗口（非实时）'} · ${fmt(snapshot.sourceSampleCount, 0)}点 · ${fmt(snapshot.availableWindowMs, 1)} ms${detail ? '；独立于本地历史细节图' : ''}`;
    $('metricScope').textContent = scope; $('measurementScope').textContent = scope;
    for (const id of ['pauseDisplay', 'resumeDisplay', 'exportCapture', 'armTrigger']) $(id).disabled = !online;
    if (!pendingSelect && pendingWindow === null && document.activeElement !== $('windowSelect')) $('windowSelect').value = snapshot.windowMs;
    $('latestEvent').textContent = snapshot.events?.[0] ? `${snapshot.events[0].time} · ${snapshot.events[0].text}` : '当前没有事件';
    $('footerStats').textContent = `统计起点 ${snapshot.stats.startedAt} · 累计 ${fmt(snapshot.stats.frames, 0)} 帧 · 缺口 ${snapshot.stats.gaps ?? '未知'} · 发送丢帧 ${snapshot.streamDrops ?? '未知'} · 界面上限5Hz · ${snapshot.lastSampleTime || '尚无样点时间'}`;
  }
  function requestRender() {
    if (scheduled || document.hidden || !snapshot) return;
    scheduled = true;
    // 接收和校验保留每条WS消息，绘图最多5Hz合并到最新快照，降低多画布的CPU/GPU开销。
    renderTimer = setTimeout(() => requestAnimationFrame(() => {
      renderTimer = null; scheduled = false; if (closed || document.hidden || !snapshot) return;
      const start = performance.now();
      lastRender = start;
      try { refreshChannels(); fillTrigger(); renderStatus(); renderCharts(); renderMeasurements(); renderEvents(); renderDiagnostics(); }
      catch (error) { notice('页面绘图失败：' + error.message); console.error(error); }
      perf.rendered++; push(perf.renderMs, performance.now() - start); push(perf.delayMs, performance.now() - latestPerformanceAt);
    }), Math.max(0, RENDER_INTERVAL_MS - (performance.now() - lastRender)));
  }
  let latestPerformanceAt = performance.now();
  function connect() {
    if (closed || document.hidden || socket?.readyState === WebSocket.OPEN || socket?.readyState === WebSocket.CONNECTING) return;
    socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/`);
    socket.onmessage = event => {
      const at = performance.now();
      try {
        const data = JSON.parse(event.data);
        if (data.type === 'snapshot') {
          M.validateSnapshot(data); perf.messages++; if (scheduled) perf.coalesced++;
          const key = M.session(data.frozen ? data.captureMeta : data.meta);
          if (lastMetaKey !== null && key !== lastMetaKey) { clearDetail(); view = null; cursors = [null, null]; chartKey = ''; }
          if (snapshot && (JSON.stringify(data.selected) !== JSON.stringify(snapshot.selected) || data.windowMs !== snapshot.windowMs || data.frozen !== snapshot.frozen)) {
            clearDetail(); view = null; cursors = [null, null]; chartKey = '';
          }
          lastMetaKey = key; snapshot = data; latestAt = Date.now(); latestPerformanceAt = at;
          if (pendingSelect && pendingSelect.every((v, i) => data.selected[i] === v) && data.selected.length === pendingSelect.length) { pendingSelect = null; legendKey = ''; }
          if (pendingWindow === data.windowMs) pendingWindow = null;
          requestRender();
        } else if (data.type === 'error') { pendingSelect = null; pendingWindow = null; legendKey = ''; notice(data.text); }
        else if (data.type === 'exported') notice(`录波已保存：${data.name}，请在“录波与事件”下载CFG / DAT`);
      } catch (error) { perf.invalidMessages++; notice('已拒绝无效快照：' + error.message); }
    };
    socket.onclose = () => { if (snapshot) renderStatus(); if (!closed && !document.hidden) reconnect = setTimeout(connect, 1500); };
    socket.onerror = () => { if (snapshot) renderStatus(); };
  }
  const checkFreshness = () => { if (!document.hidden && snapshot) renderStatus(); };
  let freshTimer = setInterval(checkFreshness, 1000);
  document.addEventListener('visibilitychange', () => {
    clearTimeout(reconnect);
    if (document.hidden) { socket?.close(); } else { connect(); requestRender(); }
  });
  window.addEventListener('pagehide', () => { closed = true; clearInterval(freshTimer); clearTimeout(reconnect); clearTimeout(workerTimer); clearTimeout(renderTimer); scheduled = false; socket?.close(); clearDetail(); resize.disconnect(); chartKey = ''; destroyCharts(); });
  window.addEventListener('pageshow', event => {
    if (!event.persisted) return;
    closed = false; freshTimer = setInterval(checkFreshness, 1000); resize.observe(document.querySelector('.content-panel')); connect(); requestRender();
  });
  // 只读性能端点供浏览器验收使用，不暴露全部通道数据，也不修改现场计数。
  window.t153WorkspaceDiagnostics = (includeSamples = false) => ({ page, layout, physical, selected: snapshot?.selected, visible: snapshot?.selected.filter(i => !hidden.has(i)),
    messages: perf.messages, rendered: perf.rendered, renderLimitHz: 1000 / RENDER_INTERVAL_MS, invalidMessages: perf.invalidMessages, coalesced: perf.coalesced,
    renderMs: summary(perf.renderMs), delayMs: summary(perf.delayMs), samples: includeSamples ? { renderMs: [...perf.renderMs], delayMs: [...perf.delayMs] } : undefined, longTasks: perf.longTasks, longTaskMs: perf.longTaskMs,
    heapBytes: performance.memory?.usedJSHeapSize ?? null, charts: charts.map(c => ({ min: c.scales.x.min, max: c.scales.x.max, points: c.data[0].length })),
    rawReport, detail: detail ? { sourceCount: detail.sourceCount, displayCount: detail.displayCount, decimated: detail.decimated, savedAt: detail.savedAt } : null,
    sourceCount: snapshot?.sourceSampleCount, displayCount: snapshot?.displaySampleCount, meta: snapshot ? { period: snapshot.meta.period, generation: snapshot.meta.generation, faults: snapshot.meta.faults } : null });
  connect();
})();
