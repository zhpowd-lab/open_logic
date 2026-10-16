// 这里只解释物理采样通道，不改变装置的保护配置或 ADC 量程。
const spectrum = require('./spectrum');
// 将公共板/回路输入映射到五片 ADC 的 A/B 交错原始索引，保留第 10 回路 Ic/I0 交换接线。
function channelIndex(slot, input) {
  const chip = slot === 0 ? 0 : 1 + Math.floor((slot - 1) / 4);
  let physical = slot === 0 ? input : ((slot - 1) % 4) * 4 + input;
  if (slot === 10 && input >= 2) physical ^= 1;
  return chip * 16 + ((physical & 7) << 1) + (physical >> 3);
}

// 生成 80 路默认名称、物理通道与标称换算系数；辅助通道保留 ADC 引脚单位，现场校准可另设。
function defaultChannels() {
  const channels = Array.from({ length: 80 }, (_, index) => ({
    index, name: `ADC${Math.floor(index / 16) + 1} 辅助${Math.floor(index % 16 / 2) + 1}${index % 2 ? 'B' : 'A'}`,
    group: '公共板', phase: '', circuit: 'PUBLIC', unit: 'V',
    a: 10 / 32768, b: 0, calibration: 'adc_pin',
    physical: `ADC${Math.floor(index / 16) + 1} ${index % 2 ? 'B' : 'A'}${Math.floor(index % 16 / 2)}`,
  }));
  // 当前独立采集全部为 ±10 V；工程量系数按原应用及板级电路换算到这一量程。
  const voltageGain = (100 / 51000) * (1 + 330 / (3.9 + 24.9));
  // 前八路名称按 equ_cfg_5adc_16sw.bin；实际相电压或线电压接线仍需现场确认。
  const voltageNames = ['公共1 Uab', '公共1 Ubc', '公共1 Uca', '公共1 U0',
    '公共2 Uab', '公共2 Ubc', '公共2 Uca', '公共2 U0', '公共备用1', '公共备用2'];
  for (let input = 0; input < 10; input++) {
    const ch = channels[channelIndex(0, input)];
    Object.assign(ch, { name: voltageNames[input], a: 10 / 32768 / voltageGain,
      phase: input < 8 ? ['AB', 'BC', 'CA', '0'][input % 4] : '', calibration: 'nominal', circuit: 'PUBLIC_VOLTAGE' });
  }
  for (let slot = 1; slot <= 16; slot++) {
    ['Ia', 'Ib', 'Ic', 'I0'].forEach((phase, input) => {
      const ch = channels[channelIndex(slot, input)];
      // 三相沿用 SCALE_CT_10A_604；I0 按 5 A/2.5 mA、1.4 kΩ 负载计算。
      const a = input === 3 ? 10 / (32768 * 0.7) : 10 / (0.201521 * 32768 / 3.125);
      Object.assign(ch, { name: `回路${String(slot).padStart(2, '0')} ${phase}`,
        group: `回路${String(slot).padStart(2, '0')}`, phase, circuit: `SW${slot}`,
        unit: 'A', a, calibration: 'nominal' });
    });
  }
  return channels;
}

// 校验通道表长度、索引、名称与有限正比例，保留固定物理映射；无效配置抛错而不部分应用。
function validateChannels(values) {
  if (!Array.isArray(values) || values.length !== 80) throw new Error('通道配置必须包含 80 路');
  const defaults = defaultChannels();
  return values.map((value, index) => {
    if (value.index !== index || typeof value.name !== 'string' || !value.name.trim() ||
        value.name.length > 64 || /[,\r\n]/.test(value.name) || !['A', 'V', 'code'].includes(value.unit) ||
        !Number.isFinite(value.a) || value.a <= 0 || value.a > 1000000 ||
        !Number.isFinite(value.b) || Math.abs(value.b) > 1000000 ||
        !['nominal', 'manual', 'adc_pin'].includes(value.calibration))
      throw new Error(`通道 ${index + 1} 配置无效`);
    return { ...defaults[index], name: value.name.trim(), unit: value.unit,
      a: value.a, b: value.b, calibration: value.calibration };
  });
}

// 按当前帧采集片掩码判断物理通道是否存在；旧流兼容五片，不把未使能补零当有效读数。
function channelValid(frame, index) {
  return !!((frame?.enabledChipMask ?? 0x1f) & (1 << Math.floor(index / 16)));
}

// 对有效窗口计算原始码及工程量 RMS/DC/峰峰值，连续且有足够幅度才估计频率；计数不足返回无值。
function measure(frames, ch) {
  // 未采集的通道不能把补零解释为真实零值，也不能与有效样点混合计算。
  if (!frames.length || frames.some(frame => !channelValid(frame, ch.index)))
    return { index: ch.index, count: 0 };
  let min = Infinity, max = -Infinity, sum = 0, squares = 0;
  const rawValues = frames.map(f => f.raw.readInt16LE(8 + ch.index * 2));
  for (const value of rawValues) { min = Math.min(min, value); max = Math.max(max, value); sum += value; squares += value * value; }
  const dc = sum / frames.length, variance = Math.max(0, squares / frames.length - dc * dc);
  // 有足够幅度并且时间、序号连续时才估计频率，零漂不显示为工频。
  const crossings = [], hysteresis = (max - min) * 0.15;
  let low = false, continuous = true;
  for (let i = 1; i < frames.length; i++) {
    if (frames[i].n !== ((frames[i - 1].n + 1) >>> 0) || frames[i].us <= frames[i - 1].us) continuous = false;
    if (rawValues[i] < dc - hysteresis) low = true;
    if (low && rawValues[i] >= dc && rawValues[i - 1] < dc) {
      const ratio = (dc - rawValues[i - 1]) / (rawValues[i] - rawValues[i - 1]);
      crossings.push(frames[i - 1].us + ratio * (frames[i].us - frames[i - 1].us)); low = false;
    }
  }
  const frequency = continuous && max - min >= 8 && crossings.length >= 3 ?
    1000000 * (crossings.length - 1) / (crossings.at(-1) - crossings[0]) : null;
  // FFT 只用连续窗口；明确估算/无效质量不能当真实波形，旧流缺质量字段保留 unknown。
  const sampleQuality = frames.some(frame => frame.flags !== undefined &&
    (!Number.isInteger(frame.flags) || frame.flags < 0 || frame.flags > 0xffffffff || !(frame.flags & 1) || (frame.flags & (1 << 11)))) ? 'invalid' :
    frames.some(frame => frame.flags === undefined) ? 'unknown' : 'adc_valid_flag';
  const fft = !continuous || sampleQuality === 'invalid' ?
    { fundamentalRms: null, thdPercent: null, reason: !continuous ? 'discontinuous' : 'invalid_quality' } :
    spectrum.measure(frames.map(frame => frame.us / 1000), rawValues);
  // 小于八码的窗不能把量化零漂标成工频基波；RMS 仍保留原窗口的真实数值。
  if (max - min < 8 && !fft.reason) { fft.fundamentalRms = null; fft.thdPercent = null; fft.reason = 'low_amplitude'; }
  fft.sampleQuality = sampleQuality; fft.amplitudeUnit = 'code';
  const physicalDc = dc * ch.a + ch.b;
  return { index: ch.index, count: frames.length, latestRaw: rawValues.at(-1), minRaw: min, maxRaw: max,
    rmsRaw: Math.sqrt(squares / frames.length), dcRaw: dc, ppRaw: max - min,
    latest: rawValues.at(-1) * ch.a + ch.b, min: min * ch.a + ch.b, max: max * ch.a + ch.b,
    rms: Math.sqrt(variance * ch.a * ch.a + physicalDc * physicalDc),
    acRms: Math.sqrt(variance) * ch.a, dc: physicalDc, pp: (max - min) * ch.a, frequency,
    fundamentalRms: fft.fundamentalRms === null ? null : fft.fundamentalRms * ch.a,
    thdPercent: fft.thdPercent, fft };
}

module.exports = { channelIndex, defaultChannels, validateChannels, measure, channelValid };
