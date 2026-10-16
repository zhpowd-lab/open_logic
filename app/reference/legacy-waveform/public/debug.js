// 页面生命周期只在可见且已启用时安排下一次读取；隐藏与关闭立即中止当前请求。
function createRefreshLoop({ request, onResult, onError, onBusy = () => {},
  visible = () => true, schedule = setTimeout, cancel = clearTimeout }) {
  let auto = false, closed = false, timer = null, active = null;
  function stop() {
    if (timer !== null) cancel(timer);
    timer = null;
    if (active) { active.controller.abort(); active = null; onBusy(false); }
  }
  function next() {
    if (!closed && auto && visible() && !active && timer === null)
      timer = schedule(() => { timer = null; refresh(); }, 2000);
  }
  function refresh() {
    if (closed || !visible()) return Promise.resolve(null);
    if (active) return active.promise;
    if (timer !== null) cancel(timer);
    timer = null;
    const state = { controller: new AbortController() };
    active = state; onBusy(true);
    state.promise = Promise.resolve().then(() => request(state.controller.signal)).then(value => {
      if (active === state && !closed && visible()) onResult(value);
      return value;
    }, error => {
      if (active === state && !state.controller.signal.aborted) onError(error);
      return null;
    }).finally(() => {
      if (active === state) { active = null; onBusy(false); next(); }
    });
    return state.promise;
  }
  return { refresh, setAuto(value) {
    auto = !!value;
    if (auto && visible() && !closed) refresh();
    else if (timer !== null) { cancel(timer); timer = null; }
  }, visibilityChanged() {
    if (!visible()) stop();
    else if (auto) refresh();
  }, close() { closed = true; auto = false; stop(); },
    reopen() { closed = false; auto = false; } };
}

// 标签导航只改变可见分组，刷新不会重建标签或触发额外诊断请求。
function createTabNavigation(tabs, panels) {
  let current = 0;
  function select(index, focus = false) {
    current = (index + tabs.length) % tabs.length;
    tabs.forEach((tab, i) => {
      tab.setAttribute('aria-selected', String(i === current));
      tab.tabIndex = i === current ? 0 : -1;
      panels[i].hidden = i !== current;
    });
    if (focus) tabs[current].focus();
  }
  tabs.forEach((tab, i) => {
    tab.addEventListener('click', () => select(i));
    tab.addEventListener('keydown', event => {
      const index = { ArrowRight: current + 1, ArrowLeft: current - 1,
        Home: 0, End: tabs.length - 1 }[event.key];
      if (index !== undefined) { event.preventDefault(); select(index, true); }
    });
  });
  select(0);
  return { select, get activeIndex() { return current; } };
}

const LABELS = {
  errors: '采集错误总数', faults: '采集故障位图',
  first_error: '首个采集错误', missed_cycles: '漏转换周期', read_errors: '总线读取错误',
  ring_errors: '共享环错误', busy_timeouts: 'BUSY超时', qualified: '启动连续资格帧',
  mask: 'ADC片掩码', read_count: '总线读取次数', read_max_ticks: '总线最大耗时 / tick',
  read_sum_ticks: '总线累计耗时 / tick', sample_sequence: '采集末帧序号', age_ms: '报告年龄 / ms',
  controller_alive: '实际控制端持有资源', fresh: '当前报告新鲜有效',
  lbc_runtime_writable: '运行时配置开放', lbc_generation: 'LBC配置代次', lbc_applied: 'ARM前寄存器已读回',
  lbc_clock_hz: 'LBC时钟 / Hz', lbc_select_clock_hz: '选片等待时钟 / Hz', lbc_select_ticks: 'GPIO稳定等待 / tick',
  lbc_rd_cycle: '片内读周期计数', lbc_cs_on: 'CS起始计数', lbc_cs_off: 'CS结束计数',
  lbc_oe_on: 'RD起始计数', lbc_oe_off: 'RD结束计数', lbc_access: '读数据采样计数',
  lbc_cs_reg: '实际CS寄存器', lbc_oe_reg: '实际RD寄存器', lbc_cycle_reg: '实际读写周期寄存器',
  lbc_access_reg: '实际数据采样寄存器', lbc_status_error: 'LBC配置查询错误',

  pid: 'Linux PID', reader_max_wake_lateness_ns: '采集最大晚唤醒',
  reader_max_cycle_ns: '采集最大完整周期', protection_max_body_ns: '保护最大业务计算',
  protection_max_cycle_ns: '保护最大完整周期',
  period_ns: '线程周期', tid: 'Linux TID', policy: '实际调度策略', priority: '实际优先级',
  startup_cpu: '启动时 CPU', affinity_cpu_count: '亲和 CPU 数', affinity_verified: '亲和性核实',
  max_wake_lateness_ns: '最大晚唤醒', max_read_ns: '最大共享批读取', max_release_ns: '最大批释放',
  max_cycle_ns: '最大完整周期耗时', max_validation_ns: '最大帧校验',
  max_queue_copy_ns: '最大应用队列复制', skipped_deadlines: '跳过的唤醒期限',
  read_calls: '批读取调用次数', source_frames: '累计源帧', source_errors: '源读取错误',
  rejected_frames: '拒绝帧', transient_read_retries: '暂态读取重试', transient_release_retries: '暂态释放重试',
  max_transient_stall_ns: '最大暂态等待', max_import_ns: '最大业务样点导入',
  max_import_wait_ns: '采集等待业务缓冲', max_buffer_wait_ns: '保护等待业务缓冲',
  buffer_import_owner: '负责保护和录波数据导入',
  max_body_ns: '最大保护业务计算', max_delivery_age_ns: '最大数据交付年龄',
  maximum_delivery_age_ns: '允许数据交付年龄', calls: '保护调用次数', inhibited: '闭锁保护次数',
  capacity: '队列容量', high_water: '最大积压帧', ring_min_headroom: '共享环最小余量',
  queued_frames: '累计入队', imported_frames: '累计导入', discarded_after_fault: '故障后丢弃', empty_batches: '空批次',
  pipeline: '采集链错误码', business: '业务错误码', detail_valid: '首错详情有效',
  reason: '首错原因', value_ns: '首错实测值', limit_ns: '首错阈值',
  period_ticks: '采样周期 / tick', clock_hz: '硬件时钟 / Hz', sample_rate_hz: '实际配置采样率 / Hz',
  chip_mask: 'ADC 片选掩码', host_epoch: 'Linux 会话', remote_epoch: '采集核会话',
  run_generation: '运行代次', config_generation: '配置代次',
  mode: '数据运输', mmap_enabled: 'mmap 已启用', mmap_region: 'mmap 区域', map_bytes: 'mmap 字节数',
  diagnostic: '诊断模式', commissioning: '调试运行模式', physical_outputs_enabled: '物理动作出口启用', sampling_fault_hold: '采样故障闭锁／仅保持维护',
  latest_sequence: '业务末帧序号', latest_ticks: '业务末帧硬件 tick', latest_time_epoch: '业务时间代次',
  sample_callbacks: '样点回调次数', protection_callbacks: '保护回调次数', auxiliary_updates: '辅助量更新',
  auxiliary_unavailable: '辅助量未知次数', gate_adc_sync: 'ADC 同步门控', gate_sw_init: '业务初始化门控',
  bus_di_raw: '本地16路开入原始电平', bus_di_valid: '本地开入有效位图', bus_di_updates: '本地开入导入帧数',
  configured_loops: '配置回路数', loop_started: '16 回路开始次数', loop_completed: '16 回路完成次数',
  budget_yields: '业务预算让出次数', loop_out_of_range: '回路越界次数',
  status_result: '硬件查询结果', status_valid: '硬件状态有效', running: '平台运行标记',
  watchdog_faults: '看门狗故障位', watchdog_enabled: '看门狗启用', pit_error: 'PIT 错误码',
  reset_requested: '已请求复位', first_reset_reason: '首个复位原因', first_reset_monotonic_ns: '首个复位单调时标',
  bus: 'CAN 总线', ready: '传输已就绪', allowed_bus_mask: '允许总线掩码', last_error: '末次历史错误',
  action_fault: '动作闭锁错误', tx_longframes: '已入队长帧', tx_shortframes: '已提交短帧',
  rx_longframes: '收到完整长帧', rx_shortframes: '收到短帧', tx_dropped: '发送丢弃', rx_dropped: '接收丢弃',
  tx_queue_full: '驱动暂满次数', tx_other_errors: '发送其他错误', hw_bus_errors: 'CAN错误帧',
  malformed_frames: '不合法短帧', reassembly_timeouts: '长帧重组超时', pending_shortframes: '待发短帧',
  action_enqueued: '动作长帧入队', action_submitted: '动作长帧已提交驱动', action_rejected: '出口拒绝动作',
  action_expired: '排队超时动作', action_enqueue_failed: '动作入队失败', action_submit_max_ns: '最大动作排队到提交',
  action_dispatch_calls: '原开出轮询次数', action_dispatch_failed: '原开出轮询失败', last_dispatch_error: '末次开出轮询错误',
  last_action_monotonic_ns: '末次动作单调时标', last_action_slot: '末次动作物理槽',
  last_action_type: '末次动作类型', last_action_mask: '末次动作位图',
  rx_warning: '接收警告通知', rx_passive: '接收被动通知', tx_warning: '发送警告通知',
  tx_passive: '发送被动通知', bus_off: 'BUS-OFF通知', bus_error: '总线错误通知',
  protocol_error: '协议错误通知', lost_arbitration: '驱动报告仲裁错误', controller_overflow: '控制器溢出',
  last_class: '末次错误类别位图', last_dlc: '末次错误帧长度', last_data: '末次错误帧字节',
  configured: '已配置', active: '当前许可有效', deadline_ns: '许可截止单调时标',
  slot: '物理槽', on_mask: '试验允许分闸位图', off_mask: '允许复归位图', on_packets: '本条命令已预留包数',
  switch_test: '维护分合闸测试模式', close_mask: '试验允许合闸位图', close_packets: '合闸命令已预留次数',
  events: '持久化事件条数', operations: '持久化操作条数', next_event_id: '下一事件序号',
  unacknowledged: '待确认事件数', storage_overflows: 'SOE存储队列溢出', storage_error: 'SOE存储错误',
  requested: '录波请求', accepted: '录波请求受理', rejected: '拒绝次数',
  files_written: '录波写盘任务完成', file_errors: '录波写盘任务失败',
  upgrade_busy: '子板升级忙', queue_depth: '当前开入队列', queue_peak: '最大开入队列', dropped: '丢弃次数',
  state: '采集状态', epoch: '采集核 epoch', peer: 'peer 状态', reader_run_generation: '探针运行代次',
  reader_frames: 'Linux 已读取帧', reader_last_sequence: '探针末帧序号', reader_last_ticks: '探针末帧 tick',
  reader_gaps: '读帧缺口', reader_bad: '坏帧', reader_rejected_estimated: '拒绝估算帧',
  pattern_sequence: '采样序号', pattern_dropped: '采样丢弃', sample_missed: '漏采',
  adc_error: '当前 ADC 错误', adc_error_total: '本次 ADC 错误总数', adc_error_streak: '连续 ADC 错误',
  rtc_utc: 'RTC UTC秒', rtc_utc_iso: 'RTC日期 / UTC', rtc_ticks: 'RTC读回原生时标',
  rtc_reads: 'RTC完整读回次数', rtc_bus_errors: 'RTC与温度总线错误', rtc_ctrl: 'RTC控制寄存器',
  rtc_request_id: 'RTC末次写请求', rtc_request_result: 'RTC末次写请求结果',
  temperature_error: '温度读取状态', temperature_milli_c: '温度 / 毫摄氏度',
  gpadc_valid: '片内ADC有效位图', service_max_ticks: '辅助服务最大耗时 / tick',
  irq_max_ticks: '捕获中断最大耗时 / tick', publish_max_ticks: '辅助快照最大发布耗时 / tick',
  unexpected_capture: '未知捕获中断次数', ticks: '辅助快照原生时标', flags: '辅助状态位图',
  adc_estimated_frames: '估算帧', deadline_overruns: '采样超时', capture_points: '每周波点数',
  capture_period_ticks: 'E907 周期 / tick', capture_clock_hz: 'E907 时钟 / Hz',
  capture_adc_mask: 'ADC 片选掩码', adc_oversampling: '过采样配置', dma_enabled: 'DMA 模式',
  dma_queue_overflows: 'DMA 队列溢出', dma_queue_max_depth: 'DMA 最大队列深度',
  adc_diag_valid: 'ADC 诊断缓存有效', adc_diag_epoch: 'ADC 诊断缓存 epoch',
  adc_diag_serial: 'ADC 诊断缓存序号', adc_diag_age_ms: 'ADC 诊断缓存年龄 / ms',
  adc_faults: 'ADC 故障位图', rtc_bad: 'RTC 异常', app_active: '应用消费者活跃',
  sample_consumer: '实际消费者配置', sample_app_active: '应用消费者活跃',
  sample_consumer_generation: '消费者代次', sample_undelivered: '消费权切换未交付帧',
  reader_run_missed: '当前运行漏采', reader_run_gaps: '当前运行读帧缺口',
  reader_run_bad: '当前运行坏帧', reader_run_frames: '当前运行读取帧',
  reader_run_estimated: '当前运行估算帧', reader_run_rejected_estimated: '当前运行拒绝估算帧',
  reader_run_rtc_bad: '当前运行 RTC 异常', reader_stale_discarded: '跨会话陈旧帧丢弃',
  capture_trigger: '转换触发模式', busy_edges: '真实 BUSY 沿计数',
  busy_missing_cycles: 'BUSY 缺少周期', busy_duplicate_edges: 'BUSY 重复沿',
  busy_interval_max_ticks: 'BUSY 最大间隔 / tick', capture_error_policy: '已确认 ADC 故障策略',
  capture_error_policy_requested: '请求 ADC 故障策略', adc_diag_last_rc: '末次诊断错误码'
};
function displayValue(name, value) {
  if (value === null || value === undefined) return '未知';
  if (typeof value === 'boolean') return value ? '是' : '否';
  if (name === 'bus_di_raw' || name === 'bus_di_valid')
    return '0x' + Number(value).toString(16).padStart(4, '0');
  if (name === 'policy') return ({ 0: 'SCHED_OTHER (0)', 1: 'SCHED_FIFO (1)', 2: 'SCHED_RR (2)' })[value] || String(value);
  if (name === 'mmap_region' && value === 'linux_private_batch') return 'Linux 私有批缓冲区';
  if (Array.isArray(value)) return value.map((v, i) => (i + 1) + ':' + v).join(' · ');
  if (name.endsWith('_ns') && /^\d+$/.test(String(value)))
    return (Number(value) / 1e6).toFixed(6) + ' ms (' + value + ' ns)';
  return String(value);
}
function sessionKey(value, source) {
  const part = value[source];
  if (!part?.available) return null;
  const x = part.snapshot;
  return source === 'linux' ? [x.pid, x.server_generation_ns, x.sampling.host_epoch, x.sampling.remote_epoch,
    x.sampling.run_generation, x.sampling.config_generation, x.sampling.period_ticks].join('/') :
    [part.bootId, part.fields.epoch, part.fields.reader_run_generation, part.fields.capture_period_ticks,
      part.fields.sample_consumer_generation].join('/');
}
function exactDelta(previous, current, name) {
  const a = previous?.[name], b = current?.[name];
  if (!/^\d+$/.test(String(a)) || !/^\d+$/.test(String(b))) return null;
  const delta = BigInt(b) - BigInt(a);
  return delta < 0n ? null : delta.toString();
}
// 缺项不能由Number(null)或布尔转换伪造成零值、离线或有效开入。
function canNodeCells(n) {
  const show = (v, format = String) => v == null ? '未知' : format(v);
  const hex = v => '0x' + Number(v).toString(16);
  return [show(n.slot, s => s === 0 ? '公共板' : '馈线' + s), displayValue('online', n.online),
    show(n.version, v => '0x' + Number(v).toString(16).padStart(8, '0')),
    show(n.age_us, v => (v / 1000).toFixed(1) + ' ms'),
    show(n.di_count) + ' / ' + show(n.do_count),
    show(n.di_snapshot, hex) + ' / ' + (n.di_initialized == null ? '未知' : n.di_initialized ? show(n.di_model, hex) : '未初始化'),
    show(n.received) + ' / ' + show(n.published)];
}
function ioLoopCells(n) {
  const state = s => ({ 0: '未知', 1: '未置位', 2: '置位', 3: '异常' })[s] ?? '未知';
  const point = p => p ? '槽' + displayValue('slot', p[0]) + ' · 点' + displayValue('index', p[1]) +
    ' · ' + (p[2] == null ? '未知' : p[2] + 'ms') : '未配置';
  return [displayValue('loop', n.loop), state(n.closed), state(n.open), ...(n.outputs || []).map(point)];
}
function overcurrentCells(n) {
  const show = (v, f = String) => v == null ? '未知' : f(v);
  const mask = v => '0x' + Number(v).toString(16);
  const stages = values => Array.isArray(values) ? values.map((v,i) => (i+1) + ':' + show(v)).join(' · ') : '未知';
  return [show(n.switch), show(n.enabled), show(n.runtime_enabled), show(n.m2_max),
    show(n.trip_enabled_mask,mask), stages(n.pickup_m2),
    stages(n.delay_ticks?.map(v => v == null ? null : Number(v) / 1.6)), show(n.pickup_mask,mask),
    show(n.trip_mask,mask), show(n.trip_request), show(n.output_requested)];
}
function rtioCaptureCells(c, s = {}) {
  const show = v => displayValue('', v);
  let status = ({ 0: '有效', '-61': '未取得完整有效周期', '-16': '资源冲突', '-34': '周期超范围', '-5': '捕获故障' })[c.error] ?? show(c.error);
  if (c.error === 0) {
    if (s.status_result !== 0) status = '快照不可用';
    else if (!/^\d+$/.test(String(s.ticks)) || !/^\d+$/.test(String(c.last_ticks)) || BigInt(c.last_ticks) === 0n || BigInt(c.last_ticks) > BigInt(s.ticks)) status = '年龄未知';
    else if (BigInt(s.ticks) - BigInt(c.last_ticks) > (c.slot < 2 ? 48000000n : 2400000n)) status = '陈旧';
  }
  return [['GPS秒沿', 'RTC秒沿', '测频1（引脚待核）', '测频2'][c.slot] || show(c.slot),
    show(c.count), show(c.last_ticks), show(c.period), status];
}
function rtioGpadcCells(s) {
  return (s.gpadc_raw || []).map((v, i) => {
    let age = null;
    const stamp = s.gpadc_ticks?.[i];
    // 24MHz原生时标除24即微秒；缺项和未来时标不能伪造成新鲜零龄数据。
    if (/^\d+$/.test(String(s.ticks)) && /^\d+$/.test(String(stamp))) {
      const delta = BigInt(s.ticks) - BigInt(stamp);
      if (delta >= 0n && BigInt(stamp) > 0n) age = delta / 24n;
    }
    return [i, displayValue('', v), s.status_result !== 0 ? '状态不可用' : s.gpadc_valid == null ? '有效性未知' :
      !(s.gpadc_valid & (1 << i)) ? '无效' : age === null ? '年龄未知' : age > 250000n ? '陈旧' : '有效',
      age === null ? '未知' : age.toString(), displayValue('', stamp)];
  });
}
if (typeof module !== 'undefined' && module.exports)
  module.exports = { createRefreshLoop, createTabNavigation, displayValue, exactDelta, sessionKey, canNodeCells, ioLoopCells, overcurrentCells, rtioCaptureCells, rtioGpadcCells };

// 独立页面只请求 /api/debug；使用 textContent 展示远端值，避免把文本当 HTML。
if (typeof document !== 'undefined') {
  const byId = id => document.getElementById(id);
  let previous = null;
  const panelNames = ['overview', 'acquisition', 'protection', 'can', 'e907-faults', 'platform'];
  const panels = Object.fromEntries(panelNames.map(name => [name, byId('panel-' + name)]));
  createTabNavigation(panelNames.map(name => byId('tab-' + name)), Object.values(panels));
  function card(panel, title, fields, note) {
    const box = document.createElement('section'); box.className = 'card';
    const heading = document.createElement('h2'); heading.textContent = title; box.append(heading);
    if (note) { const p = document.createElement('p'); p.textContent = note; box.append(p); }
    const table = document.createElement('table');
    for (const [name, value] of Object.entries(fields)) {
      const row = document.createElement('tr'), key = document.createElement('th'), cell = document.createElement('td');
      key.scope = 'row'; key.textContent = LABELS[name] || name; cell.textContent = displayValue(name, value);
      row.append(key, cell); table.append(row);
    }
    box.append(table); panels[panel].append(box);
  }
  function rowsCard(panel, title, columns, rows, note) {
    const box = document.createElement('section'); box.className = 'card';
    const heading = document.createElement('h2'); heading.textContent = title; box.append(heading);
    if (note) { const p = document.createElement('p'); p.textContent = note; box.append(p); }
    const table = document.createElement('table'), header = document.createElement('tr');
    for (const label of columns) {
      const cell = document.createElement('th'); cell.scope = 'col'; cell.textContent = label; header.append(cell);
    }
    table.append(header);
    for (const row of rows) {
      const line = document.createElement('tr');
      for (const value of row) { const cell = document.createElement('td'); cell.textContent = value ?? '未知'; line.append(cell); }
      table.append(line);
    }
    const scroll = document.createElement('div'); scroll.className = 'table-scroll'; scroll.append(table);
    box.append(scroll); panels[panel].append(box);
  }
  function render(value) {
    Object.values(panels).forEach(panel => panel.replaceChildren());
    byId('stamp').textContent = '读取时间：' + value.readAt + ' · 目标 ' + value.device;
    const all = (value.e907.available || (value.a7?.available && value.a7.fresh)) && value.linux.available;
    byId('status').textContent = all ? '本次读取完成' : '部分或全部诊断不可用，见下方原因';
    byId('status').className = all ? 'ok' : 'warn';
    if (value.a7?.available) {
      const a = value.a7;
      card('overview', 'A7 独立核采集', { ...a.fields, fresh: a.fresh, controller_alive: a.controller_alive },
        a.fresh ? 'CPU3裸机采集；24 tick = 1µs。当前固件read耗时不含每帧固定寄存器准备，不代表完整采集周期。数值属于本次固件会话。' : '报告已失效或控制端已退出；以下是末次报告，不能作为当前采集运行证据。');
      card('e907-faults', 'A7 采集首错与完整性', Object.fromEntries(
        ['errors', 'faults', 'first_error', 'missed_cycles', 'read_errors', 'ring_errors', 'busy_timeouts'].map(k => [k, a.fields[k]])),
        '发生源错会停采；不补点，不以E907历史诊断替代。');
    }
    if (value.e907.available) {
      const f = value.e907.fields;
      card('overview', 'E907 当前运行', { state: f.state, peer: f.peer, epoch: f.epoch,
        sample_consumer: f.sample_consumer, sample_app_active: f.sample_app_active,
        capture_period_ticks: f.capture_period_ticks, adc_error: f.adc_error,
        adc_error_total: f.adc_error_total, reader_run_missed: f.reader_run_missed },
        '更多 BUSY、DMA 与九类 ADC 原因计数见 E907 故障页。');
      let note = 'bootId ' + value.e907.bootId + '；与应用快照分别读取。缺字段保持未知。';
      if (previous && sessionKey(value, 'e907') === sessionKey(previous, 'e907')) {
        const delta = exactDelta(previous.e907.fields, value.e907.fields, 'reader_frames');
        if (delta !== null) note += ' 上次读取后新增 ' + delta + ' 帧。';
      }
      const groups = { 'E907 采样与共享环': {}, 'E907 帧质量与会话': {}, '真实 BUSY / DMA 与耗时': {}, 'ADC 异步诊断缓存': {} };
      const timing = {};
      for (const [name, field] of Object.entries(value.e907.fields)) {
        if (name.startsWith('lbc_')) { timing[name] = field; continue; }
        const group = name.startsWith('adc_diag_') || name.startsWith('capture_error_policy') ? 'ADC 异步诊断缓存' :
          /^(busy_|dma_|read_|isr_|timing_|deadline_|startup_)/.test(name) ? '真实 BUSY / DMA 与耗时' :
          /^(adc_|rtc_|reader_run_|reader_gaps|reader_bad|reader_estimated|reader_rejected|sample_missed|pattern_dropped)/.test(name) ? 'E907 帧质量与会话' : 'E907 采样与共享环';
        groups[group][name] = field;
      }
      card('acquisition', 'LBC采样读时序', timing,
        '选片等待：12 tick = 0.5µs，24 tick = 1µs。LBC字段基于100MHz时钟；实际CS/RD间隔仍以示波器为准。只允许停止采集且应用释放消费权后更新，下一次ARM生效。');
      for (const [title, fields] of Object.entries(groups)) card(title === 'E907 采样与共享环' ? 'acquisition' : 'e907-faults', title, fields, title === 'ADC 异步诊断缓存' ?
        '异步缓存必须同时核对 valid、epoch、serial 和年龄；缓存追平不等于本次新增物理故障。' : title === 'E907 采样与共享环' ? note : null);
    } else if (!value.a7?.available) card('overview', '采集核诊断不可用', { error: value.e907.error });
    if (value.linux.available) {
      const x = value.linux.snapshot;
      let note = 'PID ' + x.pid + '；计数和最大值属于当前程序会话，非本次两秒窗最大值。';
      if (previous && sessionKey(value, 'linux') === sessionKey(previous, 'linux')) {
        const delta = exactDelta(previous.linux.snapshot.reader, x.reader, 'source_frames');
        if (delta !== null) note += ' 上次读取后新增 ' + delta + ' 源帧。';
      }
      card('overview', 'Linux 当前运行', { pid: x.pid, ...x.mode,
        sample_rate_hz: x.sampling.sample_rate_hz, source_frames: x.reader.source_frames,
        pipeline: x.fault.pipeline, business: x.fault.business }, note);
      card('overview', '关键耗时', {
        reader_max_wake_lateness_ns: x.reader.max_wake_lateness_ns,
        reader_max_cycle_ns: x.reader.max_cycle_ns,
        protection_max_body_ns: x.protection.max_body_ns,
        protection_max_cycle_ns: x.protection.max_cycle_ns,
        max_delivery_age_ns: x.protection.max_delivery_age_ns,
        maximum_delivery_age_ns: x.protection.maximum_delivery_age_ns },
        '最大值为当前 Linux 程序会话累计，线程周期见实际配置；有界窗口不代表长期最坏情况已通过。');
      card('acquisition', 'Linux 采集线程', x.reader, note);
      card('acquisition', '数据总线本地开入（16路）', {
        bus_di_raw: x.business.bus_di_raw, bus_di_valid: x.business.bus_di_valid,
        bus_di_updates: x.business.bus_di_updates },
        '片选5单次读取16位，原始0为低电平、1为高电平；仅valid对应位为1时有效。与CAN子板遥信分别显示，实物变位及端子映射尚待核对。');
      card('protection', 'Linux 保护线程', x.protection);
      card('acquisition', '配置与运输', { ...x.mode, ...x.sampling, ...x.transport },
        'mmap 区域是 Linux 私有批缓冲区。快照不是所有字段的同时原子截面；计数不代表物理动作验收。');
      if (x.realtime_io) {
        const { capture, gpadc_raw, gpadc_ticks, ...state } = x.realtime_io;
        const rtcSeconds = Number(state.rtc_utc);
        card('acquisition', 'A7 秒沿、RTC 与辅助采集', { ...state,
          rtc_utc_iso: state.rtc_error === 0 && Number.isSafeInteger(rtcSeconds) && rtcSeconds > 0
            ? new Date(rtcSeconds * 1000).toISOString() : '未知' },
          '原生计数器24MHz，后台和中断耗时1 tick≈41.67ns。GPS只有秒沿，不表示UTC已锁定。status_result非0时保留旧端点供诊断，不作当前有效数据。');
        rowsCard('acquisition', 'A7 硬件捕获', ['输入', '沿序号', '最后原生时标', '周期 / 1.5MHz', '状态'],
          (capture || []).map(c => rtioCaptureCells(c, state)),
          '测频1旧PB13当前为CPU调压输出，明确报告冲突；不能把未知或首沿当有效频率。');
        rowsCard('acquisition', 'A7 片内模拟量原码', ['物理槽', '原码', '数据状态', '采集年龄 / µs', '原生采集时标'],
          rtioGpadcCells(x.realtime_io),
          '槽0–3为GPADC2 ch0–3，槽4–6为GPADC0 ch4–6；仅gpadc_valid对应位有效。未确认旧逻辑7/8映射，不据此代入直流保护。');
      }
      card('platform', '队列与首错', { ...x.queue, ...x.fault });
      card('protection', '原有 16 回路业务', x.business);
      if (x.overcurrent?.valid) rowsCard('protection', '过流投入、定值与动作链',
        ['回路', '保护投入', '运行门控', '当前平方码', '三段跳闸投入', '三段门槛平方码', '三段延时 ms', '启动位图', '动作位图', '跳闸请求', '已请求出口'],
        (x.overcurrent.loops || []).map(overcurrentCells),
        '取当前运行定值及内部平方码；逻辑计数1600Hz。出口请求、CAN提交和位置返校分别核对，不等同于物理跳闸成功。');
      card('protection', 'SOE 与动作日志', x.soe || { status: '当前应用未提供' },
        '记录与物理触点返校分别验收；不清除历史事件和确认状态。');
      card('protection', '原故障录波写盘', x.record || { status: '当前应用未提供' },
        '请求受理和CFG/DAT写盘完成分别计数；提交驱动不能代替录波文件或触点验收。');
      if (Array.isArray(x.can)) for (const bus of x.can) {
        const { errors, trip_test, ...transport } = bus;
        card('can', 'CAN' + bus.bus + ' 传输与开出', transport,
          '动作超过10ms未提交则拒绝。提交驱动不代表触点动作，缺返校仍可报跳闸失败。');
        if (errors) card('can', 'CAN' + bus.bus + ' 错误帧分类', errors,
          '驱动报告的仲裁错误含厂商PEA映射，不能直接当作正常仲裁丢失；状态通知也不是新的物理ADC故障。');
        if (trip_test) card('can', 'CAN' + bus.bus + ' 单次开出试验', trip_test,
          trip_test.switch_test ? '仅指定回路的维护单点分、合脉冲各一次，60秒后闭锁；没有自动执行或网页重臂。' :
          '只允许指定保护分闸位及该回路复归；同一原命令双发，60秒后闭锁，不可网页重臂。默认没有许可。');
      }
      else card('can', 'CAN 诊断', { status: '当前应用未提供，需部署配套版本' });
      if (x.can_nodes) {
        const { nodes, ...state } = x.can_nodes;
        card('can', '子板接入与开入队列', state);
        rowsCard('can', '实际子板与开入模型', ['板卡', '在线', '版本', '报告年龄', 'DI / DO', '快照 / 模型', '变位收 / 发布'],
          (nodes || []).map(canNodeCells), '快照来自子板查询，模型来自已发布开入；公共板与馈线15共址并独立核对。');
      }
      if (x.io_routing) {
        rowsCard('can', '原配置中的动作与返校映射', ['回路', '合位', '分位', '遥控合', '遥控分', '保护合', '保护分'],
          (x.io_routing.loops || []).map(ioLoopCells),
          '点号从0开始，映射直接取当前应用配置；本页只有读取功能。');
      }
      card('platform', '平台与复位诊断', x.hardware);
    } else card('overview', 'Linux 应用诊断不可用', { error: value.linux.error, errno: value.linux.errno ?? null },
      '本次没有应用快照，旧数据不作为当前状态。');
    previous = value;
  }
  const loop = createRefreshLoop({
    visible: () => document.visibilityState === 'visible',
    request: async signal => {
      const response = await fetch('/api/debug', { signal, cache: 'no-store' });
      if (!response.ok) throw new Error('HTTP ' + response.status);
      return response.json();
    },
    onResult: render,
    onError: () => {
      byId('status').textContent = '本次请求失败；下方若有数据，属于标注时间的末次快照。';
      byId('status').className = 'warn';
    },
    onBusy: busy => { byId('refresh').disabled = busy; }
  });
  byId('refresh').addEventListener('click', () => loop.refresh());
  byId('auto').addEventListener('change', event => loop.setAuto(event.target.checked));
  document.addEventListener('visibilitychange', () => loop.visibilityChanged());
  window.addEventListener('pagehide', () => loop.close());
  // BFCache 返回只恢复手动读取资格，不能自动恢复已结束的后台请求。
  window.addEventListener('pageshow', event => {
    if (event.persisted) loop.reopen();
    // 浏览器普通后退也可能恢复表单勾选，页面实际始终从手动模式恢复。
    byId('auto').checked = false; byId('refresh').disabled = false;
  });
}
