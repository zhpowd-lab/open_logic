# GitHub 交付说明

仓库：https://github.com/zhpowd-lab/open_logic

本次交付为实验预览版 `v0.1.0-device-v15-preview`，不是实物遥信验收合格版。

## 源码与固件

- `app/logic-analyzer/`：电脑网页、独立8102服务、数据接口和测试。
- `app/device/`：CPU3裸核、Linux模块、HTTP服务、构建脚本和测试的当前源码。
- `app/device/release/logic16.elf`：实际部署v15的CPU3固件镜像，须由匹配的A7 RemoteProc装载。
- `app/device/release/logic16_access.ko`：匹配 `5.10.198-rt97-rtmpu-periph3-a7-rt` 的Linux模块。
- `app/device/release/open_logic-device-v15-20261016.tar`：实际v15运行包，未压缩，供本机BusyBox的 `tar -xf` 使用。
- `app/device/release/open_logic-device-v15-source.tar.gz`：与v15的 `SOURCE_SHA256SUMS` 全部匹配的18份源码；剔除了原包中的Python缓存。
- `app/reference/`：旧工程相关源码快照；不是完整BSP/SDK。
- `原理图/`：本项目硬件PDF原件。

当前Git源码的 `linux/service.py`、`test/service-test.py`、`test/board-test.c` 比v15增加了诊断校验/测试，未替换实机v15。原始固件与运行包保持不变，差异见 `../verification/v15-source-check.json`。发布源码ZIP包含当前Git提交；复核v15构建请使用独立的v15源码包。

本次在本机工程和VM隔离构建目录找到ELF及配套运行包，**未找到本项目的整机刷写IMG**。没有用其他硬件或旧项目的内核、设备树、根文件系统替代；复编Linux模块仍需本机/VM上的匹配SDK、内核源码与准备好的构建目录，详见 `../../app/device/README.md`。

## 校验与验证

`manifest.json` 列出纳入交付的文件、大小与SHA256。`app/device/release/SHA256SUMS` 校验实际四个运行文件；`DOWNLOAD_SHA256SUMS` 校验两个部署/源码压缩包。GitHub Release另外提供完整源码ZIP与其下载校验清单。

发布前运行：

```powershell
node.exe --test app/logic-analyzer/test/capture.test.mjs app/logic-analyzer/test/server.test.mjs app/logic-analyzer/test/device.test.mjs
```

Node此前19/19。Linux服务5/5、C板级与状态机ASan/UBSan通过；原命令与输出为 `../verification/device-tests-review.log`。本机图案浏览器36/36、软件装置源19/19、真实当前值监视12/12；验证范围及截图见 `../验收与复现.md`。发布流程重复执行离线Node、Linux服务和C测试，不对现场装置发命令。

复制到发布树的采集JSON删除了 `sourceEpoch` 会话标识；原始本地记录不改，采样字节、周期、计数、丢样及错误结果不变。保留100kHz、1MHz成功和超期前缀、CPU3固定图案等五段复核数据。过程中的旧试验包、浏览器会话缓存、Python缓存和恢复用KO不进入本次发布；恢复工具源码保留作过程记录。

## 已知边界

- 用户确认YX11、YX12、YX13中有一路实际接点不同，CPU3和网页仍为0xFFFF。尚未证明接点采集正确，不能据此判定硬件故障。
- 1MHz是读锁存器目标速率，不是光耦带宽；实测有百万点成功段，也有超期停止段，尚未证明长期稳定。90ns直接触发RD未实现。
- 485/UART和CAN解码尚在规划；未安装开机自启，未验证断电自动恢复。
- 上传与离线验证不停止网页或装置，不ARM，不更新实机固件，不修改8098、8099、6011。
