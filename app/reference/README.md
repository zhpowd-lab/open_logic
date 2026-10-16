# 旧项目相关源码与资料快照

来源：`U:\code\RT_MPU`，2026-10-16 读取。截图中“DTU5.0-T153主程序”对应 `U:\code\dtu\dtu_5.0\sh_t153`；该工程的独立采集及实时网页已发展到隔离项目 RT_MPU，因此从 RT_MPU 取得与本任务相关的现存源文件。

| 子目录 | 内容 |
|---|---|
| legacy-waveform | 8098/8099 服务、捕获/状态模块、前端、依赖清单及原说明 |
| legacy-a7/acquisition | CPU3 裸核状态机、主循环、板级读取、DI 参数和 Linux 接入 |
| legacy-a7/tests | 对应的 A7 模型/板级/时标测试 |
| legacy-a7/rt-include | 原共享帧、内存、接口头文件 |
| tools | 原生捕获与 DI 导出工具及相关测试 |
| docs | 独立 A7 基准、16 位总线遥信、实时输入迁移资料 |

这是供分析使用的**相关文件快照**，不是完整 BSP/SDK，也不是新板可直接烧录的工程。未复制大体积 SDK、构建产物、现场捕获、运行状态、凭据或自动启动配置。原件均未修改；复制清单、字节数和 SHA256 在 `agent/reference-manifest.json`。

`legacy-waveform/server.js` 保持原样，不作为新服务启动入口。旧芯片选择、ADC/BUSY/保护业务和旧板引脚不用于本阶段新网页。新服务在相邻的 `app/logic-analyzer/`，默认端口 8102。
