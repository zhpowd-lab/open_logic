# CPU3独立遥信采集

当前实验目标TQ153/T153，内核 `5.10.198-rt97-rtmpu-periph3-a7-rt`。网口2实测对应eth1、192.168.2.100/24；COM5为115200控制台。此映射只适用于本次核实的装置。

## 当前运行版本

装置目录 `/app/open_logic/releases/20261016-v15`；本机 `release/` 的四个运行文件与SHA256SUMS一致，装置上也已检查通过。CPU0～2运行Linux，CPU3运行rt_mpu_a7_rproc裸核，E907 offline。

持续监视已开启：CPU3空闲期约1kHz读当前字，服务每次GET返回最新状态，不写波形缓冲区。正式波形采集期间暂停监视，结束后恢复；总线故障停止重试。当前实物读数0xFFFF，与用户三路接点差异不符，实物正确性未通过。

`release/SOURCE_SHA256SUMS` 固定的是v15构建源。后续本地增加了GPIO probe断言，以及 `linux/service.py` 对关核失败返回值的诊断字段校验和回归检查；这三个文件与清单不同，记录在 `../../agent/verification/v15-source-check.json`。固件、资源模块、启停脚本源码仍与清单一致。最新离线测试通过；服务诊断修正尚未替换正在运行的v15，当前监视保持原版本。下次部署应使用新的版本目录，保留当前运行包。

## 模块职责与硬件约束

| 文件 | 职责 |
|---|---|
| `firmware/main.c` | CPU3状态机、24MHz Generic Timer定时轮询、可信前缀提交与当前值监视 |
| `firmware/board.c` | 16位LBC IDA接收、固定CS、输入诊断、超时与关闭恢复 |
| `linux/access.c` | 独占总线/引脚/时钟、命令代次、只读原码mmap与关核后资源归还 |
| `linux/service.py` | 独立HTTP元数据、原码、显式ARM，不承载旧逻辑应用 |
| `logic16.h` | ABI3，144字节/36个小端uint32状态字段 |
| `logic16-layout.h` | 固件、汇编、链接脚本共用固定地址与深度 |
| `run-device.sh` | 本项目的启动、状态查询、完整关闭回退 |

使用既有设备树no-map的2MiB预留区，状态页0x4420f000，原码从0x44210000开始，每点2字节、bit0=D0。Linux只读样本映射，命令/ACK分属两核，不交换指针或本机宽度类型。

PD12～19=D0～7，PA12～19=D8～15；PA21的CS初始化后保持有效，PK3～5固定译码101，PD20的WR保持高，PD21为RD。采样只发RX，不写TX FIFO，不把数据线设为输出，不逐点切CS。只在明确关闭本项目时恢复原mux、控制位和时钟。

开启监视前做一次GPIO输入对照：暂把16根数据线设输入，顺序读PA/PD后恢复LBC；控制线不动。这不是同步16位或连续采样。当前启动对照同样0xFFFF。

## 启停

```sh
sh /app/open_logic/releases/20261016-v15/run-device.sh status
sh /app/open_logic/releases/20261016-v15/run-device.sh start
sh /app/open_logic/releases/20261016-v15/run-device.sh stop
```

start要求CPU0～3原先均在线、A7/E907均offline、无残留实例且内核匹配，占用CPU3并启用当前值监视，**不会自动ARM波形段**。这会切换实验裸核与总线资源，只在已授权的装置上执行。

stop先发RemoteProc关闭，确认CPU3归还Linux后才释放资源；保留正式固件文件，只恢复原firmware选择。关闭未确认则报错并保留资源。未改内核、设备树、引导、网络或开机启动配置。

电脑网页运行/停止、暂停监视不会调用上述脚本。网页明确新建采集只发一次ARM，最多1秒/百万点；下一次明确ARM才替换旧段。

## 隔离构建

在Linux VM用匹配内核/SDK的隔离副本。脚本拒绝覆盖已有输出目录：

```sh
export A7_PREFIX='/root/下载/U/code/RT_MPU/artifacts/a7-toolchain/gcc-arm-melis-eabi-8-2019-q3-update/bin/arm-melis-eabi-'
export LINUX_PREFIX='/root/下载/U/temp/codex_t153/extracted/opt/Embedsky/tq153_linux_sdk/out/toolchain/gcc-linaro-11.3.1-2022.06-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-'
export KERNEL_SOURCE=/root/open_logic_20261016/sdk/kernel/linux-5.10-rt
export KERNEL_BUILD=/root/open_logic_20261016/build
bash build.sh /root/open_logic_20261016/release-next
```

输出含ELF、Linux模块、服务、脚本、布局/反汇编及运行/源码哈希清单。编译不部署。装置BusyBox tar不支持-z；上传未压缩.tar，用tar -xf。FTP6001上传后回读比对，再在装置执行 `sha256sum -c SHA256SUMS`。

验证见 `../../agent/装置实现与实测.md`。90ns硬件直接触发RD、长期稳定1MHz、实物接点正确性和485/CAN解码不在已通过项中。
