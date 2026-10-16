# logic16-le-v1 数据合同

## 原始样本

连续的小端 uint16，每条恰好 2 字节，没有内嵌头、指针、结构体对齐或逐路 uint16。bit0 对应 D0，bit15 对应 D15。0/1 表示原始数字电平，不在网页里倒相成业务“分/合”含义。

例：字节 `01 80 34 12` 表示样本 `0x8001`、`0x1234`。

## JSON 元数据

```json
{
  "format": "logic16-le-v1",
  "channels": 16,
  "byteOrder": "little",
  "sampleCount": 1000000,
  "samplePeriodTicks": 24,
  "clockHz": 24000000,
  "sequenceStart": 0,
  "droppedSamples": 0,
  "source": "固定测试图案"
}
```

也可用 `"samplePeriodNs": 1000` 表示周期，允许明确的有限正数。`samplePeriodTicks` 固定表示 24 MHz tick：1 tick = 125/3 ns，不将 tick 向下取整。两个周期字段同时给出时必须一致；给出 tick 时若 `clockHz` 存在必须为 24000000。

| 字段 | 口径与缺失处理 |
|---|---|
| sampleCount | 可选；存在时必须与原始文件完整长度一致，页面实际样本数可从字节数获得 |
| samplePeriodTicks / samplePeriodNs | 至少一个由数据生产方提供；都缺失时显示 unknown，横轴改用样本索引 |
| sequenceStart | 首条样本的源序号；缺失为 unknown，不默认 0；本版仅接受 JS 安全整数范围 |
| droppedSamples | 源报告的本段丢样数；缺失为 unknown，不默认 0 |
| source | 可选来源说明；缺失为 unknown |

连续、无丢样的记录中，第 i 条样本的相对时间为 `i × period`，源序号为 `sequenceStart+i`。时间原点是文件首样本，不是电脑到包时间，也不是 UTC。

若源报告 `droppedSamples > 0`，本版没有逐点时标或缺失位置表：不补点，页面提示缺失位置未知，游标时间标记为“名义”，首样本之后的真实源序号显示 unknown。后续真实采集适配必须先核对连续性；需要表达不连续段时另行定义分段/缺口合同，不能把旧有实际不等间隔 tick 的帧直接压成假定连续的本文件。

采样周期缺失时，“运行”只用 100,000 点/秒演示本地回放进度；页面明确提示，采样周期和样本时间仍为 unknown。

## 深度

同时受三项限制：最多 1,000,000 点、最多 2,000,000 原始字节、已知周期时最多 1 秒。保留点数为 `min(原始点数, 1000000, floor(1e9 / periodNs))`。

超过上限只保留**最早的首段**并明确提示，不取末段，不循环覆盖。源原始总点数记为 `sourceSampleCount`；未载入的尾部记为 `excludedSamples`，与源报告的 `droppedSamples` 分开。

满深度后回放停止；运行按钮禁用，必须明确选择“从头回放”才能重播同一段。原始内存段保留。对于超过深度的本地文件，程序只读取最多前 2 MB，不整文件分配内存。

## 本机 HTTP

| 接口 | 返回 |
|---|---|
| `GET /api/capture` | JSON 元数据、深度信息、`mode=local-replay`、`deviceConnected=false` |
| `GET /api/samples` | 已保留段的小端 uint16 二进制 |

本机回放模式在启动时固定源快照，两个接口读取同一段，仅接受GET/HEAD。

## 装置源与显式采集

使用 `node server.mjs --device http://192.168.2.100:8102` 时，电脑只监听127.0.0.1:8102，通过独立装置服务读取CPU3原码，不经过旧waveform、诊断或6011。

| 电脑接口 | 装置接口与行为 |
|---|---|
| `GET /api/capture` | 装置同名接口；mode=device、deviceConnected=true，含段状态和独立实时值 |
| `GET /api/samples?capture=…&epoch=…&count=…` | 该段已提交的前count点，恰好count×2字节 |
| `POST /api/device/arm?period=24&count=1000000&pattern=0` | 转发装置POST /api/arm；仅显式新建采集调用 |

ARM需要 `X-Logic16-Arm: 1`、空正文；电脑检查来源和参数，装置再检查CPU3/E907、资源与命令状态。period是24MHz tick，最小24；深度不超过1秒/百万点；pattern=0是实物总线、1是CPU3固定图案。202只表示命令接受，完成与否要看captureState、pendingCommand、errorCode。

captureId与sourceEpoch共同标识当前段，后者来自本次CPU3启动tick。原码复制前后均核对，两者变化、点数未提交、旧请求均返回冲突，不混段。GET不创建采集。运行/停止/单次只控制网页显示与原码搜索，没有网页STOP装置命令。

未ARM时可有0点，周期/深度/首序号/丢样为null/unknown。ARM后sequenceStart=0是段内序号，不是设备永久计数。运行期只交付核验过的连续前缀，满深度停采；粘滞总线错误舍弃未确认批次。errorCode=6为期限错过，报告尾部未完成数量；错误3/4/5不能定位丢点数，droppedSamples=null，不填0。

装置声明nominalTime=true、sampleTimeBasis=software-read-entry、droppedAtTail=true。游标用index×period的名义相对时间；firstSampleTick/lastSampleTick以十进制字符串保留完整tick，最小/最大入口间隔、最大延后和读取耗时另列统计。没有逐点RD硬件时标。确认尾部停止时，已提交前缀的段内序号仍有效。

## ABI3当前值监视

CPU3共享状态版本3，固定144字节/36个小端uint32，原码格式不变。live_status低16位是当前值、bit16有效、bit17启用、高8位独立错误；服务解包为：

| 字段 | 含义 |
|---|---|
| liveMonitorSupported | 实物支持独立监视，服务软件源不支持 |
| liveWord | 最近成功原始uint16，无有效读取时null |
| liveReadSequence | 独立监视计数，uint32可回卷，不是波形段序号 |
| livePeriodTicks | 24000，空闲期目标1ms，不是1MHz波形周期 |
| liveErrorCode / liveMonitorEnabled | 独立错误/启用状态，总线错误后停止重试 |
| startupGpioProbeWord | 开启监视前一次PA/PD输入对照，不是同步连续采样 |

网页200ms GET；计数3秒不推进则提示过期。暂停仅冻网页，恢复继续GET。正式波形采集期间监视保持前值并标明暂停，之后恢复；监视不写冻结样本，不改该段错误；启动对照也不填入样本区。
