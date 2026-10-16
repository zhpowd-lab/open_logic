# 第三方资料与依赖

运行时只用 Node.js 标准库与浏览器原生 Canvas，没有新增绘图库。

- Lucide **0.468.0**：从已有 `U:\code\RT_MPU\waveform\node_modules\lucide\dist\umd\lucide.min.js` 原样复制到 `public/vendor/lucide.min.js`。采用原工程同版本；原许可证保存在 `public/vendor/LICENSE`（ISC，含 Feather 的 MIT 来源说明）。
- 原工程还使用 uPlot 1.6.31、fft.js 4.0.4、ws 8.18.0。本页数字阶梯由原生 Canvas 绘制，不需要模拟曲线/FFT/WebSocket 包。
- PulseView、DSView、libsigrokdecode 仅用于功能调研；公开资料和下载的参考源码保存在 `agent/research/`，原版权与许可证保留，没有引入页面或运行时。详见 `agent/功能规划与开源依据.md`。
