// 同一频谱接口同时供 Node 与浏览器使用，底层 FFT 来自固定开源依赖，避免两端不同算法。
(function (root) {
  const FFT = typeof module === 'object' ? require('fft.js') : root.FFT;
  // 取最多 2048 个末尾点作去 DC/Hann 窗实数 FFT；只接受有限、递增且近似均匀的时标，不插补。
  function transform(timeMs, samples) {
    let size = 1;
    while (size * 2 <= samples.length && size < 2048) size *= 2;
    if (size < 16 || timeMs.length !== samples.length) return null;
    const times = timeMs.slice(-size), input = samples.slice(-size);
    if (times.some(value => !Number.isFinite(value)) || input.some(value => !Number.isFinite(value))) return null;
    const step = (times.at(-1) - times[0]) / (size - 1) / 1000;
    if (!Number.isFinite(step) || step <= 0 || times.some((value, i) => i &&
        Math.abs((value - times[i - 1]) / 1000 - step) > step * .1)) return null;
    const mean = input.reduce((a,b) => a+b, 0) / size;
    let gain = 0, energy = 0, squares = 0;
    for (let i = 0; i < size; i++) {
      const window = .5 - .5 * Math.cos(2 * Math.PI * i / (size - 1));
      const value = input[i] - mean;
      input[i] = value * window; gain += window; energy += window * window; squares += value * value;
    }
    const fft = new FFT(size), out = fft.createComplexArray();
    fft.realTransform(out, input);
    return { size, step, gain, energy, out, acRms: Math.sqrt(squares / size) };
  }
  // 单边峰值谱按实际 Hann 增益校正；这是 peak 幅值，不能直接当成有效值或完整窗口 RMS。
  function spectrum(timeMs, samples) {
    const result = transform(timeMs, samples);
    if (!result) return null;
    const { size, step, gain, out } = result, frequency = [], amplitude = [];
    for (let i = 0; i <= size / 2; i++) {
      frequency.push(i / (size * step));
      amplitude.push((i === 0 || i === size / 2 ? 1 : 2) * Math.hypot(out[i*2], out[i*2+1]) / gain);
    }
    return [frequency, amplitude];
  }
  // 标称 50 Hz 的三格频带能量估计：去 DC/Hann，至少四周波给基波、十周波给 2～25 次 THD。
  // 不追踪实际频率；有限窗、频率偏离和非谐波噪声影响结果，不能替代保护算法或物理 ADC 验收。
  spectrum.measure = function (timeMs, samples) {
    const result = transform(timeMs, samples);
    if (!result) return { fundamentalRms: null, thdPercent: null, reason: 'invalid_window' };
    const { size, step, energy, out, acRms } = result, binHz = 1 / (size * step), nominalHz = 50;
    const info = { sampleCount: size, windowMs: size * step * 1000, sampleRateHz: 1 / step,
      binHz, nominalHz, method: 'hann_three_bin_energy', fundamentalRms: null, thdPercent: null,
      harmonicCount: 0, reason: null, thdReason: null };
    if (size * step < 4 / nominalHz - 1e-9) return { ...info, reason: 'short_window' };
    // 正频率 Parseval 能量使用窗平方和归一，避免将频谱峰值误当 RMS；DC/Nyquist 不混入谐波频带。
    function bandPower(harmonic) {
      const center = Math.round(harmonic * nominalHz / binHz);
      if (center - 1 <= 0 || center + 1 >= size / 2) return null;
      let power = 0;
      for (let i = center - 1; i <= center + 1; i++) power += out[i*2] ** 2 + out[i*2+1] ** 2;
      return 2 * power / (size * energy);
    }
    const fundamental = bandPower(1);
    if (fundamental === null) return { ...info, reason: 'unresolved_fundamental' };
    info.fundamentalRms = Math.sqrt(fundamental);
    if (info.fundamentalRms <= Math.max(1e-12, acRms * 1e-4)) {
      info.fundamentalRms = null; return { ...info, reason: 'no_fundamental' };
    }
    if (size * step < 10 / nominalHz - 1e-9) return { ...info, thdReason: 'short_window' };
    let harmonics = 0;
    for (let harmonic = 2; harmonic <= 25; harmonic++) {
      const power = bandPower(harmonic);
      if (power === null) break;
      harmonics += power; info.harmonicCount++;
    }
    if (info.harmonicCount < 24) return { ...info, thdReason: 'insufficient_bandwidth' };
    info.thdPercent = 100 * Math.sqrt(harmonics / fundamental);
    return info;
  };
  if (typeof module === 'object') module.exports = spectrum;
  else root.rtSpectrum = spectrum;
})(globalThis);
