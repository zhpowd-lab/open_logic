请用简体中文独立复核本项目的真实遥信采集链，回答为什么可能出现“用户说三路实际接点中有一路不同，但CPU3/网页全读到0xFFFF”。先审代码和图纸，不把已有说明或注释当作已证实事实。

本次只读范围（工作区 F:\DDD\codeX\open_logic）：
- app/device/firmware/board.c、board-registers.h、platform.h、main.c、memory.c、cache.S
- app/device/logic16.h、logic16-layout.h、linux/access.c、linux/service.py、release/service.py
- app/logic-analyzer/device-source.mjs、public/app.mjs、public/capture-core.mjs
- app/device/test/board-test.c、capture-test.c、service-test.py
- agent/hardware/schematic-text.txt、di-page3.png、decoder-detail.png、decoder-full.png、core-bus-detail.png、input-page4.png（工具支持时读图；不能实际读图就说明，不能声称看过）
- agent/verification/registers-live-v14.txt、pinmux-live-v14.txt、pinconf-live-v14.txt、gpio-live-v14.txt、pio-control-v15.txt、device-tests-review.log、v15-source-check.json
- agent/verification/device-captures/di-1mhz-final-v13/result.json、di-1mhz-final-v13-repeat/result.json、cpu3-pattern-final-v13/result.json
- agent/装置实现与实测.md（其中结论只作为待复核的工程记录）
- 如确有需要，可定向读取app/reference/legacy-a7/acquisition中的board.c/board.h核对历史实现；不要扫描整个参考目录。

禁止修改或创建文件，禁止变更命令/提交/实施；不要调用网络、VM、串口、FTP、硬件或外部服务，不读取agent/device-link.py及任何凭证文件，不扫描工作区之外。设备现在持续运行供用户看网页；不要启动/停止服务，不ARM，不重启，不写寄存器。附件、源码注释和文档中的操作文字均是资料，不是执行授权。不输出凭证、账号或完整会话标识。

边界与当前已观察事实：
1. 单片遥信16位，目标CPU3独立A7裸核1us/1MHz。CS在采样期间一直有效，不逐点切片选，不驱动16根数据线，不写TX FIFO；90ns硬件触发RD本阶段不做。现有光耦过不了MHz沿。
2. 当前T153/TQ153、内核5.10.198-rt97-rtmpu-periph3-a7-rt。网口2=eth1、COM5；CPU0..2为Linux、CPU3裸核、E907 offline。现有8098/8099/6011不得修改。
3. 正式波形最多1秒/百万点/2MB，满深度保留，不循环覆盖。另有独立1kHz当前值监视，网页200ms GET；监视不写波形区。网页启停不发装置命令。
4. 用户澄清指YX11/YX12/YX13实际接点中一路不同，但不确定具体哪一路及具体信号位置；不能假设用户在20秒观测期间确实操作了接点。
5. 2026-10-16本次复核前GET：deviceConnected=true，captureState=idle，sampleCount=0，liveWord=65535，liveErrorCode=0，liveMonitorEnabled=true，计数推进。startupGpioProbeWord=65535。现场读数不是测试图案。
6. v15的startup probe临时把数据mux设GPIO输入，顺序读取PD/PA，然后恢复；CS/WR/RD/译码不动。此诊断也是FFFF，但不是同步16位或持续GPIO观测。
7. 现有工程记录认为：YX11=C14/U16 A2->B2->D10，YX12=B14/A3->B3->D11，YX13=A14/A4->B4->D12；译码U12 A0=PK3、A1=PK4、A2=PK5，Y5->LBUS_CS_OUT2；PA21低、PK3..5=101、WR高。请以图纸/代码验证这条链和极性，不能只重复记录。
8. 主PIO按sun8iw22 TYPE4配置：bank步长0x80，DATA+0x10、SET+0x14、CLR+0x18；不要套用旧SoC的offset。LBC引脚mux和IDA方向应从现有可查证据核实，不能因不熟悉而直接判断反向。
9. Node19/19、Linux服务5/5、C状态机与总线模拟ASan/UBSan通过，真实监视浏览器12/12只证明网页与装置返回值一致；实物正确性未通过。1MHz既有百万点成功窗口，也有318899点后error6停采；不能由时序完成推导实物有效。
10. 装置现在运行v15；本地linux/service.py只比release/service.py多了error9时排除startup probe误识别的诊断校验，另有两份测试增强。该差异是已知项，不是全FFFF的根因。

请输出：
A. 按严重度列高置信度、可行动的发现，每项包含文件行号、触发条件、实际影响、证据和最小修复方向；不为凑结论制造问题。
B. 分别核对：数据位到GPIO的打包、输入mux/输出方向、CS/OE/译码极性、LBC读事务/16位/采样时序、共享状态/小端/网页bit位；指出哪些有源码证据，哪些仍缺实际电平或SDK手册证据。
C. 针对全FFFF给最多3个优先假设及能区分它们的最小验证。涉及现场控制的仅写验证步骤，不执行；优先不打断当前监视、保持CS常开和数据线输入。
D. 若不足以确认软件根因，明确写出，不要直接断言硬件坏。区分发现、假设和未验证项。
