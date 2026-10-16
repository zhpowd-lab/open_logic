# 波形前端第三方来源

本次新增页面继续使用原锁定依赖，没有新增npm依赖或复制参考项目实现。

| 依赖 | 固定版本 | 许可证 | 用途 |
| --- | --- | --- | --- |
| uplot | 1.6.31 | MIT，保留node_modules/uplot/LICENSE | 两版页面绘图 |
| fft.js | 4.0.4 | package.json声明MIT | 原FFT打包/计算函数 |
| ws | 8.18.0 | MIT，保留node_modules/ws/LICENSE | 原本机WebSocket服务 |
| lucide | 0.468.0 | ISC，LICENSE包含Feather部分MIT来源 | 原版图标；新页不使用 |

公开源码研究及固定提交、许可证原文和文件SHA见
[调研记录](../docs/T153-Web双布局与公开源码调研-20261014.md)和
[来源清单](../docs/evidence/web-workspace-20261014/public-sources.json)。

PlotJuggler（MPL-2.0）、Open MCT（Apache-2.0）、openComtradeViewer（GPL-3.0）只用于阅读设计；未将这些项目代码加入本项目。uPlot同步API参考固定1.6.31版本，独立实现本项目的数据连接、开关分组和交互。
