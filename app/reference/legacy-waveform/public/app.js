// 集中按 ID 获取已有页面元素，避免各交互重复选择器约定。
const $ = id => document.getElementById(id);
const colors = ['#61d5ba', '#f2bd58', '#da83b1', '#7fb5f1', '#ef856d', '#bfd777', '#b7a2df', '#c1ced6'];
const calibrationNames = { nominal: '标称 / 未校准', manual: '手工设置', adc_pin: 'ADC引脚' };
let plot, snapshot, socket, fftMode = false, engineering = false, view = {}, lastKey = '', rendering = false;
let channelKey = '', initialized = false, zoomAxis = 'x', cursorTarget = null, cursorValues = [null, null];
let plottedData, plottedChannels, plottedUnits, noticeTimer;
// 格式化有限测量值为紧凑文本，缺值显示占位；只影响展示精度，不改变原始数据。
const format = value => Number.isFinite(value) ? (Math.abs(value) >= 10000 ? value.toFixed(0) : value.toFixed(4).replace(/\.?0+$/, '')) : '—';
// 替换短时提示并取消旧提示计时器，防止多个 ACK/错误提示定时器互相覆盖。
const notice = text => { clearTimeout(noticeTimer); $('message').textContent = text; $('message').style.display = 'block'; noticeTimer = setTimeout(() => $('message').style.display = 'none', 5000); };
// 把上位机显示/触发命令经本机 WebSocket 发给服务；未连接时提示，不直接操作装置 ADC。
function send(value) { if (socket?.readyState === 1) socket.send(JSON.stringify(value)); else notice('电脑监测服务未连接'); }
// 替换工具按钮图标并由 Lucide 重建 SVG，保留按钮本身的事件与可访问名称。
function iconButton(element, name) { element.replaceChildren(); const i = document.createElement('i'); i.dataset.lucide = name; element.append(i); lucide.createIcons(); }

for (let slot = 1; slot <= 16; slot++) {
  const option = document.createElement('option'); option.value = option.textContent = `回路${String(slot).padStart(2, '0')}`; $('group').append(option);
}
lucide.createIcons();

// 配置或分组改变时重建通道选项，保留编辑选择；勾选只提交显示通道，不改变硬件掩码。
function refreshChannels() {
  const key = JSON.stringify([snapshot.channels, snapshot.selected, $('group').value]);
  if (key === channelKey) return;
  channelKey = key;
  for (const id of ['channel', 'profileChannel']) {
    const previous = $(id).value;
    $(id).replaceChildren(...snapshot.channels.map(ch => {
      const o = document.createElement('option'); o.value = ch.index; o.textContent = `${ch.name} · ${ch.physical}`; return o;
    }));
    $(id).value = previous || snapshot.config.channel;
  }
  $('selectedCount').textContent = `${snapshot.selected.length} / 8`;
  const group = $('group').value;
  $('channelList').replaceChildren(...snapshot.channels.filter(ch => group === 'all' || ch.group === group).map(ch => {
    const label = document.createElement('label'), check = document.createElement('input'), swatch = document.createElement('i'), text = document.createElement('span');
    check.type = 'checkbox'; check.checked = snapshot.selected.includes(ch.index); check.setAttribute('aria-label', ch.name);
    check.onchange = () => {
      const selected = check.checked ? [...snapshot.selected, ch.index] : snapshot.selected.filter(i => i !== ch.index);
      if (!selected.length || selected.length > 8) { check.checked = !check.checked; return notice('请选择 1～8 路波形'); }
      view = {}; send({ type: 'select', selected });
    };
    swatch.className = 'swatch'; const position = snapshot.selected.indexOf(ch.index);
    if (position >= 0) swatch.style.background = colors[position];
    text.textContent = ch.name; label.title = `${ch.physical} · ${calibrationNames[ch.calibration]}`; label.append(check, swatch, text); return label;
  }));
}

// 仅接受有限且递增的轴范围后更新图表比例，避免无效缩放污染视图。
function applyScale(chart, key, min, max) {
  if (!Number.isFinite(min) || !Number.isFinite(max) || max <= min) return;
  chart.setScale(key, { min, max });
}
// 以鼠标位置或轴中心按 factor 缩放选定 X/Y 轴；不修改后台采样或时间戳。
function zoom(factor, clientX, clientY) {
  if (!plot) return;
  const rect = plot.over.getBoundingClientRect();
  for (const key of Object.keys(plot.scales)) {
    if (key === 'x' ? zoomAxis === 'y' : zoomAxis === 'x') continue;
    const s = plot.scales[key];
    const center = key === 'x' && clientX !== undefined ? plot.posToVal(clientX - rect.left, 'x') :
      key !== 'x' && clientY !== undefined ? plot.posToVal(clientY - rect.top, key) : (s.min + s.max) / 2;
    applyScale(plot, key, center - (center - s.min) * factor, center + (s.max - center) * factor);
  }
}
// 为图表安装滚轮、中键平移、框选/游标和复位交互；destroy 回调移除全局监听防止重复绑定。
function mouseControls(chart) {
  const over = chart.over;
  let pan = null;
  // 鼠标滚轮围绕光标缩放已选择轴并阻止页面滚动。
  const wheel = event => { event.preventDefault(); zoom(event.deltaY < 0 ? 0.8 : 1.25, event.clientX, event.clientY); };
  // 中键建立平移初态；布置游标时左键优先消耗事件，避免同时启动框选。
  const down = event => {
    if (event.button === 1) {
      event.preventDefault(); event.stopImmediatePropagation();
      pan = { x: event.clientX, y: event.clientY, scales: Object.fromEntries(Object.entries(chart.scales).map(([k, s]) => [k, { min: s.min, max: s.max }])) };
    } else if (event.button === 0 && cursorTarget !== null) {
      event.preventDefault(); event.stopImmediatePropagation();
      cursorValues[cursorTarget] = chart.posToVal(event.clientX - over.getBoundingClientRect().left, 'x');
      cursorTarget = null; $('cursorA').classList.remove('active'); $('cursorB').classList.remove('active'); chart.redraw(); updateCursor();
    }
  };
  // 拖动期间按初始轴范围和屏幕位移计算新范围，避免逐帧累计误差。
  const move = event => {
    if (!pan) return;
    const r = over.getBoundingClientRect();
    for (const [key, s] of Object.entries(pan.scales)) {
      if (key === 'x' ? zoomAxis === 'y' : zoomAxis === 'x') continue;
      const delta = (key === 'x' ? (pan.x - event.clientX) / r.width : (event.clientY - pan.y) / r.height) * (s.max - s.min);
      applyScale(chart, key, s.min + delta, s.max + delta);
    }
  };
  // 结束平移并释放本次鼠标状态，不改变冻结/采集状态。
  const up = () => { pan = null; };
  // 双击恢复当前数据的自动视图，后台历史和触发保持。
  const reset = () => { view = {}; chartData(); };
  over.addEventListener('wheel', wheel, { passive: false }); over.addEventListener('mousedown', down, true);
  over.addEventListener('dblclick', reset); document.addEventListener('mousemove', move); document.addEventListener('mouseup', up);
  (chart.hooks.destroy ||= []).push(() => {
    over.removeEventListener('wheel', wheel); over.removeEventListener('mousedown', down, true); over.removeEventListener('dblclick', reset);
    document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up);
  });
}
// 在当前可见横轴范围绘制 A/B 游标，并按屏幕像素比处理画布坐标。
function drawCursors(chart) {
  const ctx = chart.ctx, ratio = devicePixelRatio;
  ctx.save(); ctx.setLineDash([5 * ratio, 4 * ratio]); ctx.font = `${11 * ratio}px sans-serif`;
  cursorValues.forEach((value, index) => {
    if (value === null || value < chart.scales.x.min || value > chart.scales.x.max) return;
    const x = chart.valToPos(value, 'x', true); ctx.strokeStyle = ctx.fillStyle = index ? '#f2bd58' : '#ef856d';
    ctx.beginPath(); ctx.moveTo(x, chart.bbox.top); ctx.lineTo(x, chart.bbox.top + chart.bbox.height); ctx.stroke();
    ctx.fillText(index ? 'B' : 'A', x + 4 * ratio, chart.bbox.top + 14 * ratio);
  }); ctx.restore();
}
// 按游标最近样点显示主通道数值、时间/频率差；时域两游标还能给出周期频率。
function updateCursor() {
  if (!plot || !plottedData?.[0].length || cursorValues.every(v => v === null)) { $('cursorReadout').textContent = ''; return; }
  const values = cursorValues.map(value => {
    if (value === null) return null;
    const index = plot.valToIdx(value); return { x: plottedData[0][index], y: plottedData[1][index] };
  });
  const unit = plottedUnits[0], axisUnit = fftMode ? 'Hz' : 'ms';
  const parts = values.map((v, i) => v && `${i ? 'B' : 'A'} ${format(v.x)} ${axisUnit} / ${format(v.y)} ${unit}`).filter(Boolean);
  if (values[0] && values[1]) {
    const dx = values[1].x - values[0].x;
    parts.push(`Δ${fftMode ? 'f' : 't'} ${format(dx)} ${axisUnit} / Δ值 ${format(values[1].y - values[0].y)} ${unit}`);
    if (!fftMode && dx) parts.push(`${format(1000 / Math.abs(dx))} Hz`);
  }
  $('cursorReadout').textContent = parts.join(' · ');
}
// 将当前快照转原始码或工程量曲线，必要时计算 FFT；配置变化才重建图，普通刷新保留用户缩放。
function chartData() {
  if (!snapshot?.data[0].length) {
    // 空窗口移除旧曲线及游标，避免换会话后把历史图当作当前读回。
    plot?.destroy(); plot = null; lastKey = ''; cursorValues = [null, null];
    $('extent').textContent = '暂无样点'; $('cursorReadout').textContent = ''; $('spectrumPeak').textContent = '';
    return;
  }
  rendering = true;
  try {
    const channels = snapshot.selected.map(i => snapshot.channels[i]), units = channels.map(ch => engineering ? ch.unit : 'code');
    // 长窗口时域可抽点保留峰值，FFT 必须使用后台提供的连续原始末尾样点。
    const source = fftMode ? (snapshot.fftData || snapshot.data) : snapshot.data;
    let data = [source[0], ...channels.map((ch, i) => engineering ? source[i + 1].map(v => v === null ? null : ch.a * v + ch.b) : source[i + 1])];
    if (fftMode) {
      const spectra = data.slice(1).map(series => rtSpectrum(data[0], series));
      if (spectra.some(s => !s)) {
        // 频谱输入不足或无效时移除旧图，不能用上一轮频谱冒充当前窗口。
        plot?.destroy(); plot = null; lastKey = ''; cursorValues = [null, null];
        $('extent').textContent = '频谱不可计算'; $('cursorReadout').textContent = '';
        $('spectrumPeak').textContent = '样点或时标不足 / 无效'; return;
      }
      data = [spectra[0][0], ...spectra.map(s => s[1])];
      const amplitudes = data[1]; let index = 1;
      for (let i = 2; i < amplitudes.length; i++) if (amplitudes[i] > amplitudes[index]) index = i;
      $('spectrumPeak').textContent = `${channels[0].name} · 主峰 ${format(data[0][index])} Hz / ${format(amplitudes[index])} ${units[0]}pk`;
    } else $('spectrumPeak').textContent = '';
    const key = JSON.stringify([fftMode, engineering, channels, zoomAxis]);
    const rect = $('plot').getBoundingClientRect(), uniqueUnits = [...new Set(units)];
    plottedData = data; plottedChannels = channels; plottedUnits = units;
    if (!plot || key !== lastKey) {
      plot?.destroy(); lastKey = key; view = {}; cursorValues = [null, null];
      const scales = { x: { time: false } };
      // 时域保留有符号样点并围绕零点自动缩放；频谱沿用非负幅值范围。
      uniqueUnits.forEach(unit => { scales[unit] = { auto: true, range: (_chart, min, max) => {
        if (fftMode) return uPlot.rangeNum(min, max, 0.1, true);
        const peak = Math.max(Math.abs(min ?? 0), Math.abs(max ?? 0)) || 1;
        return [-peak * 1.1, peak * 1.1];
      } }; });
      plot = new uPlot({ width: Math.floor(rect.width) - 2, height: Math.floor(rect.height) - 58, scales,
        cursor: { drag: { x: zoomAxis !== 'y', y: zoomAxis !== 'x', setScale: true } },
        hooks: { ready: [mouseControls], draw: [drawCursors], setScale: [(chart, key) => {
          if (!rendering) view[key] = { min: chart.scales[key].min, max: chart.scales[key].max };
        }] },
        series: [{ label: fftMode ? 'Hz' : 'ms' }, ...channels.map((ch, i) => ({ label: ch.name, scale: units[i], stroke: colors[i], width: 1.2,
          value: (_, value) => `${format(value)} ${units[i]}` }))],
        axes: [{ stroke: '#abb3ba', grid: { stroke: '#2b3035' }, label: fftMode ? '频率 / Hz' : '时间 / ms' },
          ...uniqueUnits.map((unit, i) => ({ scale: unit, side: i % 2 ? 1 : 3, stroke: '#abb3ba', grid: { show: i === 0, stroke: '#2b3035' },
            label: unit === 'code' ? 'ADC 原始码' : `${unit}${fftMode ? ' / peak' : ''}`, size: 65 }))],
      }, data, $('plot'));
      fitPlot();
    } else {
      plot.setData(data, !Object.keys(view).length);
      for (const [key, scale] of Object.entries(view)) plot.setScale(key, scale);
    }
    fitPlot();
    const durationMs = snapshot.data[0].at(-1) - snapshot.data[0][0];
    const count = Number.isInteger(snapshot.sourceSampleCount) ? snapshot.sourceSampleCount : snapshot.data[0].length;
    const display = snapshot.displayDecimated ? ` · 曲线峰值抽点 ${snapshot.data[0].length.toLocaleString()} 点` : '';
    const history = Number.isFinite(snapshot.historyDurationMs) ? ` · 实际历史 ${format(snapshot.historyDurationMs)} ms` : '';
    $('extent').textContent = `${count.toLocaleString()} 原始样点 · 当前窗 ${format(durationMs)} ms${display}${history}`;
    updateCursor();
  } finally { rendering = false; }
}
// 按容器及图例实际尺寸调整画布，保留最小高度，支持全屏与窗口变化。
function fitPlot() {
  if (!plot) return;
  const rect = $('plot').getBoundingClientRect(), legend = plot.root.querySelector('.u-legend');
  const height = legend ? legend.getBoundingClientRect().height : 0;
  const size = { width: Math.floor(rect.width) - 2, height: Math.max(180, Math.floor(rect.height - height) - 8) };
  if (plot.width !== size.width || plot.height !== size.height) plot.setSize(size);
}
// 只按服务快照确认显示/触发状态；冻图、后台读回和浏览器断连分别呈现。
function displayState(data, connected = true) {
  const hasSamples = !!data.data[0].length, streaming = data.status === 'online';
  let text, state, detail;
  if (!connected) {
    text = '显示未更新 · 服务断线'; state = 'disconnected';
    detail = '浏览器与本机服务已断开，当前图和测量保留末次数据；重连后等待服务确认状态。';
  } else {
    if (data.resumeOnData) {
      text = '等待自动恢复'; state = 'waiting';
      detail = '断线窗口已保留；收到新的有效样点后自动恢复显示。按暂停显示可保留此窗口。';
    } else if (data.frozen) {
      text = '显示已暂停'; state = 'paused';
      detail = '当前波形和测量固定在冻结窗口。按继续恢复实时显示并取消布防。';
    } else if (data.capturing) {
      text = streaming && hasSamples ? '正在记录后触发' : '后触发记录等待数据'; state = 'recording';
      detail = (streaming && hasSamples ? '正在收集触发后数据，记录完成后冻结显示。' : '后触发记录尚未结束，等待新的样点。') +
        '按继续会结束本次记录并取消布防。';
    } else if (streaming && hasSamples) {
      text = data.armed ? '实时显示 · 已布防' : '实时显示'; state = 'live';
      detail = data.armed ? '波形和测量随新数据更新；满足触发条件后记录并冻结。按继续取消布防。' :
        '波形和测量随新数据更新。按暂停显示只固定当前窗口。';
    } else {
      text = hasSamples ? '显示未更新' : '等待读回数据'; state = 'waiting';
      detail = hasSamples ? '当前图和测量保留末次窗口。' : '尚无可显示的样点。';
    }
    if (data.frozen && data.capturing) detail += ' 后触发记录尚未结束，完成后将显示记录窗口。';
    else if (data.frozen && data.armed) detail += ' 触发仍已布防。';
    detail += streaming ? ' 后台仍接收装置读回数据。' :
      data.status === 'offline' ? ' 波形连接已断开，后台正在重连。' :
      data.status === 'stalled' ? ' 连接存在，但样点未推进。' :
      data.status === 'fault' ? ' 当前读回状态异常，请查看运行状态。' : ' 暂未收到新的读回数据。';
  }
  $('displayState').textContent = text; $('displayState').dataset.state = state;
  $('displayDetail').textContent = detail;
  const continuous = connected && streaming && hasSamples && !data.frozen && !data.capturing && !data.armed && !data.resumeOnData;
  for (const [id, active] of [['resume', continuous], ['pause', !!data.frozen && !data.resumeOnData], ['arm', !!data.armed]]) {
    $(id).classList.toggle('active', connected && active);
    $(id).setAttribute('aria-pressed', String(connected && active));
    $(id).disabled = !connected || (id === 'pause' && ((!hasSamples) || (data.frozen && !data.resumeOnData)));
  }
  $('resume').title = data.capturing ? '结束当前后触发记录，恢复实时显示并取消布防' : '恢复实时显示并取消布防';
  const window = data.frozen ? '冻结窗口' : connected && streaming && hasSamples ? '实时窗口' : '末次窗口';
  $('measurementWindow').textContent = hasSamples ? `${window}${connected ? '' : ' · 服务断线'} · ${format(data.data[0].at(-1))} ms` : '暂无样点';
}
// 展示后台测得的原始码/工程量指标及数字量，未知用占位，通道颜色与图一致。
function measurements() {
  const constant = snapshot.measurements.filter(m => m.count >= 2 && Number.isFinite(m.minRaw) && m.minRaw === m.maxRaw);
  const notices = [];
  if (constant.length) notices.push(`本窗口原始码未变化：${constant.map(m => snapshot.channels[m.index].name).join('、')}。这只描述当前窗口；若现场输入变化而读回不变，仍需排查采集链。`);
  if (snapshot.selected.some(index => snapshot.channels[index].calibration === 'nominal'))
    notices.push('标称通道按预设系数换算，尚未现场标定；A / V 数值不是已验收实测值。');
  $('measurementNotice').textContent = notices.join(' ');
  $('measurements').replaceChildren(...snapshot.measurements.map((m, i) => {
    const ch = snapshot.channels[m.index], row = document.createElement('tr');
    const reasons = { invalid_window: '时标/窗口无效', short_window: '窗口不足', unresolved_fundamental: '频率分辨率不足',
      no_fundamental: '未分辨基波', low_amplitude: '幅度不足', discontinuous: '样点不连续', invalid_quality: '样点质量无效' };
    const fftWindow = m.fft?.sampleCount ? `${m.fft.sampleCount}点 / ${format(m.fft.windowMs)} ms` : '—';
    const fftState = m.fft?.reason ? reasons[m.fft.reason] || m.fft.reason : m.fft?.thdReason ? 'THD窗口或带宽不足' : '';
    const values = [ch.name, m.latestRaw, `${format(m.latest)} ${ch.unit}`, `${format(m.rms)} ${ch.unit}`,
      `${format(m.acRms)} ${ch.unit}`, `${format(m.fundamentalRms)} ${ch.unit}`, `${format(m.thdPercent)} %`,
      `${format(m.dc)} ${ch.unit}`, `${format(m.min)} ${ch.unit}`, `${format(m.max)} ${ch.unit}`, `${format(m.pp)} ${ch.unit}`,
      format(m.frequency), calibrationNames[ch.calibration],
      !m.count ? '无有效样点' : m.count < 2 ? '样点不足' : !Number.isFinite(m.minRaw) || !Number.isFinite(m.maxRaw) ? '—' : m.minRaw === m.maxRaw ? '原始码恒定' : '原始码有变化',
      [fftWindow, fftState, m.fft?.sampleQuality === 'unknown' ? '质量标记未知' : ''].filter(Boolean).join(' · ')];
    row.append(...values.map((v, column) => {
      const td = document.createElement('td'); td.textContent = v ?? '—';
      if (!column) { const swatch = document.createElement('i'); swatch.className = 'swatch'; swatch.style.background = colors[i]; td.prepend(swatch); }
      if (column === 12 || column === 13) td.className = 'measurement-state'; return td;
    })); return row;
  }));
  $('digital').replaceChildren(...Array.from({ length: 64 }, (_, i) => {
    const span = document.createElement('span'), light = document.createElement('i'), value = snapshot.digital?.[i];
    if (value === 1) light.className = 'on'; span.append(light, `DI${i + 1} ${value ?? '—'}`); return span;
  }));
}
// 将有效板端 UTC 字符串显示为 UTC+8 日历时间；无锚点明确显示未同步。
function absoluteTime(value) {
  if (!value) return '未同步';
  return new Date(value).toLocaleString('zh-CN', { timeZone: 'Asia/Singapore', hour12: false }) +
    '.' + value.slice(20, 26) + ' UTC+8';
}
// 把有限装置运行秒数格式化为小时/分钟/秒；缺值保留未知。
function uptime(seconds) {
  if (!Number.isFinite(seconds)) return '—';
  return `${Math.floor(seconds / 3600)} h ${Math.floor(seconds % 3600 / 60)} min ${Math.floor(seconds % 60)} s`;
}

// A7计数独立解释；缓存年龄、控制归属和波形会话必须同时匹配。
function a7Runtime(health, meta) {
  const report = health.a7 || {}, fields = report.fields || {};
  const elapsed = Date.now() - Date.parse(health.checkedAt);
  const integer = name => typeof fields[name] === 'string' && /^\d{1,20}$/.test(fields[name]) &&
    BigInt(fields[name]) <= 0xffffffffffffffffn ? fields[name] : null;
  const age = integer('age_ms') !== null && Number.isFinite(elapsed) && elapsed >= 0 ?
    Number(fields.age_ms) + elapsed : null;
  const known = health.online === true && health.backend === 'a7_amp' &&
    health.acquisitionOwner === 'a7_amp' && health.acquisitionFresh === true &&
    health.controllerAlive === true && report.fresh === true && report.controller_alive === true &&
    Number.isInteger(meta.epoch) && meta.epoch > 0 && meta.epoch <= 0xffffffff &&
    integer('epoch') === String(meta.epoch) && health.epoch === meta.epoch &&
    Number.isInteger(meta.period) && meta.period > 0 &&
    integer('period_ticks') === String(meta.period) && health.capture_period_ticks === meta.period &&
    age !== null && age <= 45000;
  return { known, age, fields,
    count: name => known && integer(name) !== null ? BigInt(fields[name]).toLocaleString() : '--' };
}

// 展示当前采集健康字段并兼容旧名，按实际计数频率换算；缺字段不继承冻结波形或假设为零。
function displayAdcDiagnostics(health, meta = {}) {
  if (health.backend === 'a7_amp') {
    const a7 = a7Runtime(health, meta);
    $('rReadbackState').textContent = a7.known ?
      'A7 CPU3 独立采集；E907 未参与。CS/RD 物理时序待示波器核对。' :
      'A7 读回诊断待核对：快照过期、归属或采样会话未匹配。';
    const values = [['实际读取后端', a7.known ? 'A7 IDA + PIO' : '--'],
      ['A7生产序号', a7.count('sample_sequence')], ['真实BUSY资格帧', a7.count('qualified')],
      ['五片读取次数', a7.count('read_count')], ['读取区间最长 / tick', a7.count('read_max_ticks')],
      ['实际采样率', a7.known && Number.isInteger(meta.tickHz) && meta.tickHz > 0 ?
        `${(meta.tickHz / meta.period).toFixed(3)} Hz` : '--'],
      ['说明', '读取区间不含每帧固定寄存器准备；完整故障见运行诊断页。']];
    $('adcDiagnostics').replaceChildren(...values.flatMap(([name, value]) => {
      const dt = document.createElement('dt'), dd = document.createElement('dd');
      dt.textContent = name; dd.textContent = value; return [dt, dd];
    }));
    return;
  }
  /* 这些计数来自当前采集会话，不能用冻结波形或旧版本缺字段推断为零。 */
  // 优先使用当前诊断新字段，仅在缺失时读取兼容旧字段。
  const value = (name, previous) => health[name] === undefined ? health[previous] : health[name];
  // 只展示非负安全整数计数，缺失/损坏保持未知，不能显示成假零。
  const counter = (name, previous) => {
    const count = value(name, previous);
    return Number.isSafeInteger(count) && count >= 0 ? count.toLocaleString() : '--';
  };
  // 组合两项观测计数并保留每侧未知标记，便于区分无执行与零观察。
  const pair = (first, second) => {
    const left = counter(first), right = counter(second);
    return left === '--' && right === '--' ? '--' : `${left} / ${right}`;
  };
  const trigger = value('capture_trigger', 'capture_busy_irq');
  const clock = health.capture_clock_hz;
  // 按本次 capture_clock_hz 换算时长，未知频率显示 tick，哨兵值不解释为巨大时间。
  const tickTime = ticks => !Number.isSafeInteger(ticks) || ticks < 0 || ticks === 0xffffffff ? '--' :
    Number.isInteger(clock) && clock > 0 && clock <= 0xffffffff ? `${(ticks * 1000000 / clock).toFixed(3)} μs` : `${ticks} tick`;
  const elapsed = Date.now() - Date.parse(health.checkedAt);
  // 新后端/计时只解释新鲜且同 epoch 的探针快照；旧缓存和配置请求不能确认实际 DMA。
  const runtime = health.online === true && health.acquisitionOwner === 'probe' &&
    Number.isFinite(elapsed) && elapsed >= 0 && elapsed <= 45000 &&
    Number.isInteger(meta.epoch) && meta.epoch > 0 && meta.epoch <= 0xffffffff &&
    health.epoch === meta.epoch ? health : {};
  const backend = [0, 1].includes(runtime.dma_enabled) && runtime.dma_status_error === undefined ? runtime.dma_enabled : null;
  const runtimeCounter = name => Number.isInteger(runtime[name]) && runtime[name] >= 0 &&
    runtime[name] <= 0xffffffff ? runtime[name].toLocaleString() : '--';
  const dmaCounter = name => backend === 1 ? runtimeCounter(name) : '--';
  const runtimeTick = name => Number.isInteger(runtime[name]) && runtime[name] >= 0 &&
    runtime[name] <= 0xffffffff ? tickTime(runtime[name]) : '--';
  // BUSY入口或分片IRQ次数不证明对应极值已记录；零极值保持未知，实际零计数仍保留。
  // 各段软件时间不等于 DMA 总线传输或物理沿延迟。
  const dmaTime = (name, calls) => backend === 1 && Number.isInteger(runtime[calls]) &&
    runtime[calls] > 0 && runtime[calls] <= 0xffffffff &&
    Number.isInteger(runtime[name]) && runtime[name] > 0 ? runtimeTick(name) : '--';
  const period = runtime.capture_period_ticks, sampleClock = runtime.capture_clock_hz;
  const rateKnown = Number.isInteger(period) && period > 0 && period <= 0xffffffff &&
    Number.isInteger(sampleClock) && sampleClock > 0 && sampleClock <= 0xffffffff &&
    period === meta.period && sampleClock === meta.tickHz;
  const qualification = backend === 0 ? 'BUSY PIO' : backend === 1 ? 'BUSY DMA' : '真实 BUSY';
  const stage = health.startup_stage;
  const stages = ['待开始', '配置校验', '总线初始化', 'ADC 配置', 'DMA 初始化', 'BUSY 归属',
    'PWM 启动', 'BUSY 校准', 'DMA 预读', '目标周期', '初始化完成',
    `${qualification} 前 128 帧校验（尚未上送）`, `${qualification} 启动校验完成`];
  const stageText = Number.isSafeInteger(stage) && stage >= 0 ? `${stage} · ${stages[stage] || '未知阶段'}` : '--';
  const high = counter('busy_high_observations'), low = counter('busy_low_observations');
  // 旧版调试只解释已有状态与末次共享帧；失联时不把缓存字段当成本轮采集成功。
  const debug = health.online === true ? health : {};
  const debugCounter = name => Number.isSafeInteger(debug[name]) && debug[name] >= 0 ? debug[name].toLocaleString() : '--';
  const first = Array.isArray(debug.adc_first) && debug.adc_first.length === 5 &&
    debug.adc_first.every(value => Number.isInteger(value) && value >= -32768 && value <= 32767) ? debug.adc_first : null;
  const firstText = first ? first.map((value, index) => `ADC${index + 1}: ${value}`).join(' / ') : '--';
  const readTime = Number.isSafeInteger(debug.adc_last_read_ns) && debug.adc_last_read_ns >= 0 ?
    `${(debug.adc_last_read_ns / 1000).toFixed(3)} μs` : '--';
  const checked = health.checkedAt ? new Date(health.checkedAt).toLocaleTimeString('zh-CN', { timeZone: 'Asia/Singapore', hour12: false }) : '--';
  $('rReadbackState').textContent = health.online !== true ? 'R核本轮未核对，调试字段保持未知。' :
    health.acquisitionOwner !== 'probe' ? '当前未提供独立R核读回诊断。' :
    `R核最近核对 ${checked}：${health.remoteState === 'running' ? 'R核运行' : health.remoteState || '未知'} · ADC${debug.state === 6 ? '故障停止' : debug.state === 4 ? '采集运行' : '状态 ' + (debug.state ?? '未知')} · 生产序号 ${debugCounter('pattern_sequence')} · Linux消费序号 ${debugCounter('reader_last_sequence')} · ADC2首通道 ${first ? first[1] + '码' : '--'}（末次共享帧）`;
  const values = [['R核心跳', debugCounter('heartbeat')], ['R采样回调', debugCounter('timer_irqs')],
    ['R生产序号', debugCounter('pattern_sequence')], ['Linux消费序号', debugCounter('reader_last_sequence')],
    ['共享帧首通道（五片）', firstText],
    [backend === 0 ? 'PIO最后读取耗时' : backend === 1 ? 'DMA最后读取软件区间' : '最后读取耗时（后端未知）', readTime],
    ['实际读取后端', backend === 0 ? 'PIO' : backend === 1 ? 'DMA' : '--'],
    ['配置点数 / 周波', runtimeCounter('capture_points')], ['配置标称线频 / Hz', runtimeCounter('nominal_line_hz')],
    ['实际采样率', rateKnown ? `${(sampleClock / period).toFixed(3)} Hz` : '--'],
    ['采样周期', runtimeTick('capture_period_ticks')],
    ['PIO读取预算', backend === 0 ? runtimeTick('read_budget_ticks') : '--'],
    ['BUSY采集入口次数', dmaCounter('dma_timer_calls')], ['DMA完成IRQ次数', dmaCounter('dma_irq_calls')],
    ['BUSY入口最长', dmaTime('dma_timer_max_ticks', 'dma_timer_calls')],
    ['DMA完成IRQ最长', dmaTime('dma_irq_max_ticks', 'dma_irq_calls')],
    ['DMA完成回调最长', dmaTime('dma_callback_max_ticks', 'dma_irq_calls')],
    ['DMA入口至提交最长', dmaTime('dma_pipeline_max_ticks', 'dma_timer_calls')],
    ['DMA队列最大深度', dmaCounter('dma_queue_max_depth')], ['DMA队列溢出', dmaCounter('dma_queue_overflows')],
    ['DMA发布次数', dmaCounter('dma_publish_calls')],
    ['DMA批发布周期', backend === 1 && runtime.dma_deferred_publish === 1 ? runtimeTick('dma_publish_period_ticks') : '--'],
    ['DMA批发布最长', dmaTime('dma_publish_max_ticks', 'dma_publish_calls')],
    ['读取后PWM相位 / 周期tick', debug.adc_last_phase === undefined && debug.adc_last_period === undefined ? '--' :
      `${debugCounter('adc_last_phase')} / ${debugCounter('adc_last_period')}`],
    ['采集触发', trigger === 1 ? 'BUSY 下降沿' : trigger === 0 ? 'Timer3' : '--'],
    ['采集启动', stageText], ['BUSY 高/低观察', high === '--' && low === '--' ? '--' : `${high} / ${low}`],
    ['BUSY 校准周期', counter('busy_measured_cycles')],
    ['BUSY 校准最短', tickTime(health.busy_measured_min)],
    ['BUSY 校准最长', tickTime(health.busy_measured_max)],
    ['BUSY 校准拒绝', counter('busy_measured_rejected')],
    ['普通 GPIO 高/下降沿', pair('busy_plain_high', 'busy_plain_falls')],
    ['EINT 高/挂起', pair('busy_eint_high', 'busy_eint_pending')],
    ['GPIO 转换诊断次数', counter('busy_gpio_trials')],
    ['GPIO 转换高脉宽', health.busy_gpio_high_ticks ? tickTime(health.busy_gpio_high_ticks) : '--'],
    ['GPIO 转换 BUSY 高/下降', pair('busy_gpio_high_trials', 'busy_gpio_fall_trials')],
    ['GPIO 转换诊断结果', health.busy_gpio_trials === 0 && health.busy_gpio_result === 0 ? '未执行' :
      Number.isSafeInteger(health.busy_gpio_result) ? String(health.busy_gpio_result) : '--'],
    ['BUSY 监测期限（1.5T）', tickTime(health.watchdog_period_ticks)],
    ['BUSY 提前清零次数', counter('watchdog_resets')],
    ['BUSY 监测检查次数', counter('watchdog_checks')],
    ['BUSY 缺沿超时次数', counter('watchdog_timeouts')],
    ['BUSY 旧挂起抑制次数', counter('watchdog_stale')],
    ['BUSY 中断次数', counter('busy_edges', 'busy_irq_edges')],
    ['BUSY 漏周期', counter('busy_missing_cycles')], ['BUSY 重复沿', counter('busy_duplicate_edges')],
    ['BUSY 最大间隔', tickTime(value('busy_interval_max_ticks', 'busy_irq_max_gap_ticks'))], ['ADC 错误累计', counter('adc_error_total')],
    ['ADC 连续错误', counter('adc_error_streak')], ['ADC 最大连续错误', counter('adc_error_streak_max', 'adc_error_max_streak')],
    ['ADC 估算帧', counter('adc_estimated_frames')], ['ADC 恢复次数', counter('adc_recovered_events', 'adc_recovery_events')],
    ['ADC 待恢复错误', counter('adc_pending_errors')], ['ADC 致命错误', counter('adc_fatal_errors', 'adc_fatal_streaks')]];
  $('adcDiagnostics').replaceChildren(...values.flatMap(([name, value]) => {
    const dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.textContent = name; dd.textContent = value; return [dt, dd];
  }));
}

// 运行错误来自同会话完整诊断；历史次数不决定当前采集状态，也不随网页清统计归零。
function displayRuntimeDiagnostics(data) {
  const health = data.deviceHealth || {}, meta = data.meta || {};
  if (health.backend === 'a7_amp') {
    const a7 = a7Runtime(health, meta);
    $('acquisitionMode').textContent = a7.known ? '采集模式 · A7 独立采集' : 'A7 采集模式待核对';
    const firstError = a7.fields.first_error;
    const firstKnown = a7.known && typeof firstError === 'string' && /^-?\d{1,10}$/.test(firstError) &&
      Number(firstError) >= -0x80000000 && Number(firstError) <= 0x7fffffff;
    const values = [['采集核', 'A7 CPU3'], ['诊断会话', a7.known ? a7.fields.epoch : '--'],
      ['快照年龄', a7.known ? `${(a7.age / 1000).toFixed(1)} s` : '--'],
      ['错误累计', a7.count('errors')], ['故障位图', a7.count('faults')],
      ['首个错误码', firstKnown ? firstError : '--'], ['漏周期', a7.count('missed_cycles')],
      ['读取错误', a7.count('read_errors')], ['环形缓冲错误', a7.count('ring_errors')],
      ['BUSY超时', a7.count('busy_timeouts')]];
    $('runtimeDiagnostics').replaceChildren(...values.flatMap(([name, value]) => {
      const dt = document.createElement('dt'), dd = document.createElement('dd');
      dt.textContent = name; dd.textContent = value; return [dt, dd];
    }));
    $('runtimeNote').textContent = 'A7诊断与E907历史计数分别保存。零错误仅代表本会话软件观察；不代替CS/RD时序、逐通道量值或物理保护验收。清网页统计不清装置计数。';
    return;
  }
  const unsigned = value => Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
  const checked = Date.parse(health.checkedAt), elapsed = Date.now() - checked;
  // age_ms 属于 Linux 接收完成快照时刻，叠加本机观察年龄；缺字段、过期或跨会话均未知。
  const age = unsigned(health.adc_diag_age_ms) && Number.isFinite(elapsed) && elapsed >= 0 ?
    health.adc_diag_age_ms + elapsed : null;
  const known = health.online === true && health.adc_diag_valid === 1 &&
    unsigned(meta.epoch) && meta.epoch !== 0 && health.adc_diag_epoch === meta.epoch &&
    age !== null && age <= 45000;
  const mode = known && [0, 1].includes(health.capture_error_policy) ? health.capture_error_policy : null;
  const modes = ['严格停止', '连续诊断'];
  $('acquisitionMode').textContent = mode === null ? '采集模式待核对' : `采集模式 · ${modes[mode]}`;
  const count = name => known && unsigned(health[name]) ? health[name].toLocaleString() : '--';
  const reasonNames = ['无运行错误', 'BUSY 监测超时', 'BUSY 监测重装', 'PWM 相位读取', 'BUSY 重复沿',
    'BUSY 缺周期', '样点队列', 'ADC 读取', '读取期限', '提交期限'];
  const reason = known && unsigned(health.adc_diag_last_reason) ? health.adc_diag_last_reason : null;
  const latest = reason === 0 && health.adc_diag_total !== 0 ? '未记录原因' : reasonNames[reason] || '--';
  const rc = known && Number.isInteger(health.adc_diag_last_rc) &&
    health.adc_diag_last_rc >= -0x80000000 && health.adc_diag_last_rc <= 0x7fffffff ? String(health.adc_diag_last_rc) : '--';
  const ticks = known && typeof health.adc_diag_last_ticks === 'string' && /^\d{1,20}$/.test(health.adc_diag_last_ticks) &&
    BigInt(health.adc_diag_last_ticks) <= 0xffffffffffffffffn ? `${health.adc_diag_last_ticks} tick` : '--';
  const current = data.status === 'online' && meta.state === 4 && !meta.faults ? '采集运行 · 新样点推进' :
    data.status === 'stalled' ? '样点停滞' : data.status === 'fault' ? `采集停止 · 状态 ${meta.state ?? '--'}` :
    data.status === 'offline' ? '波形断线 · 采集尚未核对' : '等待新样点';
  const values = [['当前读回', current], ['错误处理模式', mode === null ? '--' : modes[mode]],
    ['诊断会话', known ? String(health.adc_diag_epoch) : '--'], ['完整快照序号', count('adc_diag_serial')],
    ['快照年龄', known ? `${(age / 1000).toFixed(1)} s` : '--'],
    ['错误累计（饱和）', count('adc_diag_total')], ['丢弃当次事件', count('adc_diag_skipped')],
    ['恢复真实读取事件', count('adc_diag_recovered')], ['最近错误原因', latest], ['最近错误码', rc],
    ['错误软件观察时标', ticks],
    ['监测超时次数', count('adc_diag_timeout_count')], ['监测重装错误次数', count('adc_diag_reset_count')],
    ['相位读取错误次数', count('adc_diag_phase_count')], ['重复沿次数', count('adc_diag_duplicate_count')],
    ['缺周期次数', count('adc_diag_missing_count')], ['队列错误次数', count('adc_diag_queue_count')],
    ['读取错误次数', count('adc_diag_read_count')], ['读取期限次数', count('adc_diag_read_deadline_count')],
    ['提交期限次数', count('adc_diag_commit_deadline_count')]];
  $('runtimeDiagnostics').replaceChildren(...values.flatMap(([name, value]) => {
    const dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.textContent = name; dd.textContent = value; return [dt, dd];
  }));
  $('runtimeNote').textContent = !known ? '诊断快照尚未完整核对、已过期或属于其它会话；计数保持未知。' :
    (mode === 1 ? '连续诊断只继续真实 BUSY 合法读取；有运行错误不能据此通过 ADC/BUSY 或稳定性验收。' :
      mode === 0 ? '严格模式遇采集错误停止。' : '处理模式尚未核对。') +
    'DMA 各段耗时为软件观察区间，PIO 读取预算不代表 DMA 总线耗时。本机样点和间隔统计可跨 R 会话，不能代替单会话验收。次数按本固件生命周期累计；丢弃事件不等于精确漏点数，恢复事件不等于发布帧数。快照年龄含装置接收年龄与健康缓存年龄；tick 是 24 MHz 软件观察时标，不是物理沿或 UTC。清网页统计不清装置计数。心跳推进不代表新样点。';
}

// 接收本机全量快照刷新状态、时标、事件和曲线；BUSY 无物理沿捕获时隐藏旧 IRQ 响应峰值。
function display(data) {
  snapshot = data;
  if (!initialized) {
    initialized = true;
    for (const id of ['threshold', 'consecutive', 'preMs', 'postMs', 'condition']) $(id).value = data.config[id];
    $('referenceCycles').value = data.config.referenceCycles ?? 1;
    $('postCycles').value = data.config.postCycles ?? 10;
    $('postMode').value = data.config.postMode ?? 'ms';
    updateTriggerForm();
  }
  refreshChannels();
  if (document.activeElement !== $('windowMs')) $('windowMs').value = data.windowMs;
  const names = { connecting: '波形连接中', waiting: '等待波形', online: '波形在线', offline: '波形未连接', stalled: '采样停滞', fault: '采集故障' };
  $('connection').textContent = names[data.status] || data.status; $('connection').style.color = data.status === 'online' ? '#59d291' : '#ef9b81';
  const health = data.deviceHealth || {}, connection = data.connection || {};
  $('deviceHealth').textContent = health.online === true ? '装置控制台在线' : health.online === false ? '装置控制台未连通' : '装置状态待核对';
  $('deviceHealth').style.color = health.online === true ? '#59d291' : '#ef9b81';
  $('deviceHealth').title = health.checkedAt ? `最近核对 ${new Date(health.checkedAt).toLocaleString()}` : '';
  const age = connection.lastSample ? `${Math.max(0, (Date.now() - connection.lastSample) / 1000).toFixed(1)} s` : '—';
  const acquisitionLoaded = health.acquisitionLoaded ?? health.probeLoaded;
  $('connectionDetail').textContent = health.online && health.health ? `采集接口闭锁，错误 ${health.health}` :
    health.online && !acquisitionLoaded ? '装置已运行，采集模块未启动' :
    data.status === 'offline' ? `${connection.lastError || '波形连接中断'} · 自动重连中` :
    data.status === 'stalled' ? '连接仍在，采样点未推进' : '';
  // 读回来源不代表每轮硬件转换已验证；旧配套没有完整 BUSY 周期证明。
  $('source').textContent = !data.meta.epoch ? '等待数据' : data.meta.simulatedPeak ? '模拟电流 · 实际出口闭锁' : 'ADC 读回 · 待验证';
  $('address').textContent = `${data.deviceHost}:${data.devicePort}`;
  $('triggerState').textContent = data.status === 'fault' ? '采集故障 · 已停止' :
    data.status === 'stalled' ? '样点停滞' : data.status === 'waiting' || data.status === 'connecting' ? '等待数据' :
    data.capturing ? '记录后触发' : data.resumeOnData ? '等待恢复' : data.frozen ? '已冻结' :
    data.status === 'offline' ? '波形断线' : data.armed ? '已布防' : '连续显示';
  const s = data.stats, m = data.meta;
  /* BUSY 的 GPIO 入口未锁存物理沿，不能将 PWM 相位或旧窗口峰值当作响应时间。 */
  const busyEntry = (health.capture_trigger ?? health.capture_busy_irq) === 1;
  const values = [['装置运行时间', uptime(health.uptimeSeconds)], ['检测到重启', health.reboots || 0],
    ['启动编号', health.bootId ? health.bootId.slice(0, 8) : '—'],
    ['控制台核对', health.checkedAt ? new Date(health.checkedAt).toLocaleTimeString() : '—'],
    ['距最新波形', age], ['波形中断次数', connection.disconnects || 0],
    ['采集归属', health.acquisitionOwner === 'a7_amp' ? 'A7 CPU3 独立采集' : health.acquisitionOwner === 'application' ? '完整应用' : health.probeLoaded ? '独立探针' : '—'],
    ['采集接口状态', health.health === undefined ? '—' : health.health === 0 ? '正常' : `闭锁 ${health.health}`],
    ['核间状态重试', health.status_retries ?? '—'], ['实时核丢样', health.sample_dropped ?? health.pattern_dropped ?? '—'],
    ['本机累计样点数', s.frames.toLocaleString()], ['本机不连续事件', s.gaps], ['窗口发送丢帧', data.streamDrops],
    ['时钟质量', m.timeQuality === 1 ? '软件时钟 / 未 PPS 锁定' : m.timeQuality === 2 ? 'PPS 锁定' : '未同步'],
    ['时间代次', m.timeEpoch ?? '—'],
    ['RTC 进位 / 异常', `${s.rtcRollovers || 0} / ${s.rtcErrors || 0}`],
    ['本机统计起点', new Date(s.startedAt).toLocaleTimeString()],
    ['采样间隔平均', data.averageUs === null ? '—' : data.averageUs.toFixed(3) + ' μs'],
    ['间隔最小', s.minUs === null ? '—' : s.minUs.toFixed(3) + ' μs'], ['间隔最大', s.maxUs.toFixed(3) + ' μs'],
    ['窗口 IRQ 最大', busyEntry ? '未捕获物理沿' : s.irqMax === null ? '—' : (s.irqMax / 24).toFixed(3) + ' μs'],
    ['装置 IRQ 历史最大', busyEntry ? '未捕获物理沿' : m.irqMax === undefined ? '—' : (m.irqMax / 24).toFixed(3) + ' μs'],
    ['IRQ ≥ 3 μs', busyEntry ? '未测' : s.irqValid ? s.irqGe3us : '—'], ['IRQ 未知样点', s.irqUnknown], ['连接次数', s.reconnects],
    ['采集核状态 / 故障', `${m.state ?? '—'} / 0x${(m.faults || 0).toString(16)}`]];
  $('stats').replaceChildren(...values.flatMap(([name, value]) => { const dt = document.createElement('dt'), dd = document.createElement('dd'); dt.textContent = name; dd.textContent = value; return [dt, dd]; }));
  displayAdcDiagnostics(health, data.meta);
  displayRuntimeDiagnostics(data);
  const peak = busyEntry ? null : data.irqPeak;
  $('irqPeak').textContent = `最新样点 ${absoluteTime(data.latestSampleTime)}` +
    (peak ? `\n峰值样点 ${peak.sequence} · 时间代次 ${peak.timeEpoch ?? '—'} · TIMER3 ${peak.counter} · 回调时标 ${absoluteTime(peak.sampleTime)} · E907计时 ${format(Number(peak.ticks) / 24000000)} s` : '');
  $('protocol').textContent = `RWF${m.version || 1} · COMTRADE BINARY · 80A / 64D`;
  $('events').replaceChildren(...data.events.slice(0, 30).map(event => {
    const row = document.createElement('div'); row.className = 'event'; const time = document.createElement('span'), text = document.createElement('span');
    time.textContent = event.sampleTime ? absoluteTime(event.sampleTime) : new Date(event.time).toLocaleTimeString();
    time.title = `电脑观察 ${event.time}`;
    text.textContent = event.text;
    if (event.type === 'saved') for (const ext of ['cfg', 'dat', 'json']) { const a = document.createElement('a'); a.href = '/captures/' + event.text + '.' + ext; a.textContent = ext.toUpperCase(); text.append(a); }
    row.append(time, text); return row;
  }));
  displayState(data); measurements(); chartData();
}
// 连接本机 WebSocket，按回复类型刷新/提示并在关闭后重连；这是浏览器服务连接而非装置控制台。
function connect() {
  socket = new WebSocket(`ws://${location.host}`);
  // 按快照/导出/清零 ACK/错误回复分发，清零提示只在后台确认后显示。
  socket.onmessage = event => { const data = JSON.parse(event.data); if (data.type === 'snapshot') display(data); else if (data.type === 'exported') notice(`已导出 ${data.name}`); else if (data.type === 'cleared') notice('运行统计已清零，开始新监测窗口'); else if (data.type === 'error') notice(data.text); };
  // 本机网页服务断开时更新标签并延迟重连，不据此认定装置死机。
  socket.onclose = () => {
    $('connection').textContent = '电脑服务断线'; $('connection').style.color = '#ef9b81';
    displayState(snapshot || { data: [[]] }, false);
    displayAdcDiagnostics({ online: false });
    displayRuntimeDiagnostics({ status: 'offline' });
    $('rReadbackState').textContent = '电脑服务断线，R核调试待重新核对。';
    setTimeout(connect, 1500);
  };
}
$('group').onchange = () => refreshChannels();
$('windowMs').onchange = () => { view = {}; send({ type: 'select', selected: snapshot.selected, windowMs: Number($('windowMs').value) }); };
// 条件编辑只更新表单；布防才提交，本机冻结显示不停止 E907 的采集。
function updateTriggerForm() {
  const cycle = $('condition').value === 'cycle_diff';
  const edge = ['rising', 'falling'].includes($('condition').value);
  const postCycles = cycle || $('postMode').value === 'cycles';
  $('referenceCyclesField').hidden = !cycle;
  $('postMode').disabled = cycle;
  $('postMsField').hidden = postCycles; $('postCyclesField').hidden = !postCycles;
  $('thresholdLabel').textContent = cycle ? '周波绝对差 / ADC码' : '阈值 / ADC码';
  $('threshold').min = cycle ? '0' : '-32768'; $('threshold').max = cycle ? '65535' : '32767';
  $('triggerHelp').textContent = cycle ?
    '与前 1 或 2 周波同相位的原始码比较，连续 N 点绝对差超过阈值后，额外记录指定周波再暂停显示；装置采集继续。' : edge ?
    '上升或下降单次穿越沿触发，不使用连续 N 点；额外记录指定毫秒或周波，完成后暂停显示，装置采集继续。' :
    '连续 N 点满足阈值后额外记录指定毫秒或周波，完成后暂停显示；装置采集继续。';
}
$('condition').onchange = updateTriggerForm; $('postMode').onchange = updateTriggerForm;
$('arm').onclick = () => {
  const config = {};
  for (const id of ['channel', 'threshold', 'consecutive', 'preMs', 'postMs', 'referenceCycles', 'postCycles']) config[id] = Number($(id).value);
  config.condition = $('condition').value; config.postMode = $('postMode').value;
  send({ type: 'arm', config }); view = {};
};
$('resume').onclick = () => { send({ type: 'resume' }); view = {}; };
$('pause').onclick = () => send({ type: 'pause' });
// 只请求清运行统计，冻图和触发条件由后台保留，不发送装置清计数命令。
$('clearStats').onclick = () => send({ type: 'clear_stats' });
$('export').onclick = () => send({ type: 'export' });
// 先扩展页面布局再尝试浏览器全屏，内置浏览器拒绝系统全屏时仍保留占满页面。
$('fullscreen').onclick = async () => {
  const expanded = !document.body.classList.contains('expanded');
  document.body.classList.toggle('expanded', expanded);
  iconButton($('fullscreen'), expanded ? 'shrink' : 'expand');
  $('fullscreen').title = $('fullscreen').ariaLabel = expanded ? '退出全屏波形' : '全屏波形';
  try {
    if (expanded && !document.fullscreenElement) await document.documentElement.requestFullscreen();
    else if (!expanded && document.fullscreenElement) await document.exitFullscreen();
  } catch (_) { /* 内置浏览器不允许系统全屏时，保留占满页面的波形视图。 */ }
};
// 用户退出系统全屏时恢复按钮与页面布局，保持绘图区尺寸监听有效。
document.addEventListener('fullscreenchange', () => {
  if (!document.fullscreenElement) {
    document.body.classList.remove('expanded'); iconButton($('fullscreen'), 'expand');
    $('fullscreen').title = $('fullscreen').ariaLabel = '全屏波形';
  }
});
// Escape 退出页面扩展视图；有标定对话框时交由对话框处理。
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && !document.fullscreenElement && !$('profileDialog').open) {
    document.body.classList.remove('expanded'); iconButton($('fullscreen'), 'expand');
    $('fullscreen').title = $('fullscreen').ariaLabel = '全屏波形';
  }
});
// 切换时域/FFT 页并清旧缩放，重新绘制同一窗口；不改硬件采样率。
function mode(fft) { fftMode = fft; $('fftTab').classList.toggle('active', fft); $('timeTab').classList.toggle('active', !fft); view = {}; chartData(); }
$('fftTab').onclick = () => mode(true); $('timeTab').onclick = () => mode(false);
// 切换 ADC 原始码与线性工程值显示，清旧轴范围避免单位变化后错用缩放。
function units(physical) { engineering = physical; $('rawMode').classList.toggle('active', !physical); $('engineeringMode').classList.toggle('active', physical); view = {}; chartData(); }
$('rawMode').onclick = () => units(false); $('engineeringMode').onclick = () => units(true);
$('zoomIn').onclick = () => zoom(0.5); $('zoomOut').onclick = () => zoom(2); $('reset').onclick = () => { view = {}; cursorValues = [null, null]; chartData(); };
// 在 X、Y 与双轴缩放模式间循环并刷新图表交互，保留数据不改变采样率。
$('zoomAxis').onclick = () => {
  zoomAxis = { x: 'y', y: 'xy', xy: 'x' }[zoomAxis];
  $('zoomAxis').title = { x: '横向缩放', y: '纵向缩放', xy: '双轴缩放' }[zoomAxis];
  iconButton($('zoomAxis'), { x: 'move-horizontal', y: 'move-vertical', xy: 'move' }[zoomAxis]); chartData();
};
for (const [id, target] of [['cursorA', 0], ['cursorB', 1]]) $(id).onclick = () => { cursorTarget = cursorTarget === target ? null : target; $('cursorA').classList.toggle('active', cursorTarget === 0); $('cursorB').classList.toggle('active', cursorTarget === 1); };
// 将当前通道的固定物理信息和可编辑名称/系数填入对话框，修改须提交后台验证。
function fillProfile() {
  const ch = snapshot.channels[Number($('profileChannel').value)];
  $('profilePhysical').textContent = `${ch.physical} · ${ch.group}`;
  $('profileName').value = ch.name; $('profileUnit').value = ch.unit; $('profileA').value = ch.a;
  $('profileB').value = ch.b; $('profileCalibration').value = ch.calibration;
}
// 打开所选首通道的本机标定对话框，没有快照时不使用空通道。
$('settings').onclick = () => { if (!snapshot) return; $('profileChannel').value = snapshot.selected[0]; fillProfile(); $('profileDialog').showModal(); };
$('profileChannel').onchange = fillProfile; $('closeProfile').onclick = () => $('profileDialog').close();
for (const id of ['profileA', 'profileB', 'profileUnit']) $(id).oninput = () => { $('profileCalibration').value = 'manual'; };
// 复制完整通道表后提交编辑项，由后台统一校验/原子保存；固定物理映射不可在此更改。
$('profileForm').onsubmit = event => {
  event.preventDefault(); const channels = snapshot.channels.map(ch => ({ ...ch })), ch = channels[Number($('profileChannel').value)];
  Object.assign(ch, { name: $('profileName').value, unit: $('profileUnit').value, a: Number($('profileA').value), b: Number($('profileB').value), calibration: $('profileCalibration').value });
  send({ type: 'profile', channels }); $('profileDialog').close();
};
$('resetProfile').onclick = () => { send({ type: 'profile_reset' }); $('profileDialog').close(); };
new ResizeObserver(fitPlot).observe($('plot'));
connect();
