/* 双方唯一的共享内存契约；仅描述小端线格式，不放本地指针或原生枚举。 */
#ifndef RT_SHARED_ABI_H
#define RT_SHARED_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/stddef.h>
typedef u8 rt_u8;
typedef u16 rt_u16;
typedef s16 rt_s16;
typedef u32 rt_u32;
typedef s32 rt_s32;
typedef u64 rt_u64;
#else
#include <stdint.h>
#include <stddef.h>
typedef uint8_t rt_u8;
typedef uint16_t rt_u16;
typedef int16_t rt_s16;
typedef uint32_t rt_u32;
typedef int32_t rt_s32;
typedef uint64_t rt_u64;
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "RT shared ABI requires little endian"
#endif
#define RT_ALIGN64 __attribute__((aligned(64)))
#define RT_ASSERT(x, name) typedef char rt_assert_##name[(x) ? 1 : -1]
#define RT_MAGIC             0x31453752U
#define RT_ENDIAN_TAG        0x12345678U
#define RT_ABI_V1            0x00010000U
#define RT_LAYOUT_V1         1U
#define RT_MAJOR(v)          ((v) >> 16)
#define RT_REGION_BYTES      0x00080000U
#define RT_DIRECTORY_OFFSET  0x00000040U
#define RT_DIRECTORY_MAX     64U
#define RT_HOST_STATUS_OFF   0x00001000U
#define RT_REMOTE_STATUS_OFF 0x00001100U
#define RT_CONFIG0_OFF       0x00002000U
#define RT_CONFIG1_OFF       0x00004000U
#define RT_CONFIG_BANK_BYTES 0x00002000U
#define RT_COMMAND_RING_OFF  0x00008000U
#define RT_COMMAND_DATA_OFF  0x00008100U
#define RT_EVENT_RING_OFF    0x0000b000U
#define RT_EVENT_DATA_OFF    0x0000b100U
#define RT_SAMPLE_RING_OFF   0x00010000U
#define RT_SAMPLE_DATA_OFF   0x00010100U
#define RT_SAMPLE_CAPACITY   1024U
#define RT_CONTROL_CAPACITY  32U
#define RT_RECORD_BYTES     256U
#define RT_ANALOG_CHANNELS   80U
#define RT_DIGITAL_WORDS     4U

#define RT_FEAT_RAW80       (1U << 0)
#define RT_FEAT_TIME        (1U << 1)
#define RT_FEAT_CONFIG      (1U << 2)
#define RT_FEAT_PAYLOAD_CRC (1U << 3)
#define RT_FEAT_LOCAL_DI    (1U << 4)
#define RT_FEATURES_V1      0x1fU
#define RT_OBJECT_REQUIRED  (1U << 0)
#define RT_OBJ_HOST_STATUS  1U
#define RT_OBJ_REMOTE_STATUS 2U
#define RT_OBJ_CONFIG0      3U
#define RT_OBJ_CONFIG1      4U
#define RT_OBJ_SAMPLE       5U
#define RT_OBJ_COMMAND      6U
#define RT_OBJ_EVENT        7U
#define RT_FORMAT_RAW80     1U
#define RT_FORMAT_MESSAGE   2U

/* 64 位数只在已取得所有权的记录中解释；不能对两个字分别进行无保护轮询。 */
/* 固定宽度小端 64 位字对；lo/hi 在记录所有权保护下合读，不是可独立原子更新的计数器。 */
struct rt_words64 { rt_u32 lo; rt_u32 hi; };
/* 把已取得完整所有权的低/高 32 位字组合成 64 位数；不能用于无保护读取两个共享字。 */
static inline rt_u64 rt_words_value(struct rt_words64 v)
{
    return ((rt_u64)v.hi << 32) | v.lo;
}
/* 将本地 64 位数拆成小端低/高字，便于 ARM32/RV32 使用固定宽度 ABI。 */
static inline struct rt_words64 rt_words_make(rt_u64 v)
{
    struct rt_words64 w = { (rt_u32)v, (rt_u32)(v >> 32) };
    return w;
}

/* Linux 在 E907 停止、读者退出后初始化；RUN 期间整张布局表只读。 */
/* 共享区布局头，由主端独占初始化后只读；magic 最后发布，特性和布局版本决定能否互通。 */
struct rt_region_header {
    /* magic 是最后发布的就绪标记；长度与版本在任何共享访问前校验。 */
    rt_u32 magic, abi_version, header_bytes, region_bytes;
    rt_u32 endian_tag, layout_id, required_features, offered_features;
    /* 目录采用共享基址相对偏移，禁止解释成另一个核的虚拟地址。 */
    rt_u32 directory_offset, directory_count, cache_line_bytes, reserved[5];
} RT_ALIGN64;

/* next_offset 是相对于共享区首地址的偏移；目录节点按偏移顺序串联。 */
/* 共享目录节点；offset/next_offset 均相对共享区基址，bytes/element_bytes/capacity 用于边界验证。 */
struct rt_object {
    rt_u32 id, abi_version, offset, bytes;
    rt_u32 element_bytes, capacity, flags, next_offset;
};

/* 三个缓存行分别属于初始化者、唯一生产者、唯一消费者。 */
/* 只读环几何与记录格式，独占一个缓存行，生产/消费期间禁止改变。 */
struct rt_ring_descriptor {
    rt_u32 abi_version, descriptor_bytes, data_offset, capacity;
    rt_u32 record_bytes, record_format, flags, reserved[9];
} RT_ALIGN64;
/* 唯一生产者独占缓存行；write_index 为单调模 2^32 游标，dropped 是满环拒绝计数。 */
struct rt_ring_producer {
    rt_u32 write_index, dropped, reserved[14];
} RT_ALIGN64;
/* 唯一消费者独占缓存行；read_index 仅在完整复制后推进以归还槽位。 */
struct rt_ring_consumer {
    rt_u32 read_index, reserved[15];
} RT_ALIGN64;
/* 三缓存行环控制块，布局描述符、生产者和消费者互不共写，避免非一致缓存伪共享。 */
struct rt_ring_control {
    struct rt_ring_descriptor descriptor;
    struct rt_ring_producer producer;
    struct rt_ring_consumer consumer;
} RT_ALIGN64;

#define RT_SAMPLE_ADC_VALID       (1U << 0)
#define RT_SAMPLE_UTC_VALID       (1U << 1)
#define RT_SAMPLE_PPS_LOCKED      (1U << 2)
#define RT_SAMPLE_HOLDOVER        (1U << 3)
#define RT_SAMPLE_EDGE_CAPTURED   (1U << 4)
#define RT_SAMPLE_EDGE_PREDICTED  (1U << 5)
#define RT_SAMPLE_GAP_BEFORE     (1U << 6)
#define RT_SAMPLE_PAYLOAD_CRC    (1U << 7)
#define RT_SAMPLE_RATE_CHANGED   (1U << 8)
#define RT_SAMPLE_TIME_CHANGED   (1U << 9)
#define RT_SAMPLE_IRQ_DIAG       (1U << 10)
/* 短暂 ADC 错误插补值；不等同于 ADC_VALID，不用于伪造真实采样完整性。 */
#define RT_SAMPLE_ADC_ESTIMATED  (1U << 11)
#define RT_LATENCY_UNKNOWN       0xffffffffU

/* analog 保持现有录波的原始 ADC 码及总线顺序；不是可直接落盘的 COMTRADE DAT。 */
/* 固定 256 字节原始样点记录：序号、采样沿单调时标、可选 UTC、80 路 ADC 码与数字有效位；不是 COMTRADE DAT 文件。 */
struct rt_sample_v1 {
    /* 指向下一物理环槽的共享偏移；完整记录发布后才能沿链访问。 */
    rt_u32 next_offset;
    /* 单会话递增序号；缺沿、估算和重启必须通过序号/flags 明确区分。 */
    struct rt_words64 sample_sequence;
    rt_u32 session_epoch, config_generation, flags;
    /* 24 MHz 采样沿时间，不使用 ADC 读完或 Linux 收帧时间替代。 */
    struct rt_words64 sample_ticks;
    /* UTC_VALID 才能解释 UTC；软件锚点不等同于 PPS 物理锁定。 */
    struct rt_words64 utc_seconds;
    rt_u32 utc_nanoseconds, time_epoch, period_ticks;
    rt_u16 analog_count, digital_words;
    /* 片有效位描述本帧原始 ADC 来源；未知入口延迟填 RT_LATENCY_UNKNOWN。 */
    rt_u32 valid_chip_mask, irq_latency_ticks;
    /* 有符号原始 ADC 码按板端总线顺序，工程值换算由 Linux/上位机负责。 */
    rt_s16 analog[RT_ANALOG_CHANNELS];
    rt_u16 digital[RT_DIGITAL_WORDS];
    /* 数字有效位逐位说明本帧哪些状态确实读取，估算不得伪造数字边沿。 */
    rt_u16 digital_valid[RT_DIGITAL_WORDS];
    /* 可选 CRC 只覆盖 [64,240) 的模拟量、数字量及数字有效位。 */
    /* IRQ_DIAG 时 reserved 保存首次 TIMER3 计数及随后读取的 24 MHz 时钟低/高字。 */
    rt_u32 payload_crc32, reserved[3];
} RT_ALIGN64;

#define RT_STATE_RESET       0U
#define RT_STATE_HELLO       1U
#define RT_STATE_CONFIGURED  2U
#define RT_STATE_ARMED       3U
#define RT_STATE_RUNNING     4U
#define RT_STATE_STOPPED     5U
#define RT_STATE_FAULT       6U
#define RT_FAULT_ADC         (1U << 0)
#define RT_FAULT_RING_FULL   (1U << 1)
#define RT_FAULT_LATENCY     (1U << 2)
#define RT_FAULT_PROTOCOL    (1U << 3)
#define RT_TIME_INVALID      0U
#define RT_TIME_FREE_RUNNING 1U
#define RT_TIME_PPS_LOCKED   2U
#define RT_TIME_HOLDOVER     3U

/* 各端只写自己的状态；单写者 seq_lock 奇数为写入中，偶数为完整快照。 */
/* 每端独写的状态页；奇偶 seq_lock 保护完整快照，session_epoch 区分重启，reserved 保存版本约定诊断。 */
struct rt_peer_status {
    /* 奇数表示单写者更新中；读者前后读同一偶数才可接收整个状态页。 */
    rt_u32 seq_lock, bytes, state, session_epoch;
    rt_u32 heartbeat, fault_bits, active_config_generation, peer_epoch_seen;
    struct rt_words64 latest_sample_sequence;
    rt_u32 monotonic_tick_hz, irq_count, irq_max_ticks, irq_ge_3us_count;
    rt_u32 sample_drop_count, command_error_count, negotiated_features;
    rt_u32 time_epoch, time_quality, pwm_clock_hz, reserved[12];
} RT_ALIGN64;

#define RT_MSG_HELLO          1U
#define RT_MSG_APPLY_CONFIG   2U
#define RT_MSG_START          3U
#define RT_MSG_STOP           4U
#define RT_MSG_TIME_ANCHOR    5U
#define RT_MSG_READ_STATUS    6U
#define RT_MSG_CLEAR_STATS    7U
#define RT_MSG_SET_PERIOD     8U
#define RT_MSG_CLOCK_PROBE    9U
#define RT_MSG_SET_SOFT_RTC   10U
#define RT_MSG_READ_ACQ_DMA   11U
#define RT_MSG_READ_ACQ_CAPTURE 12U
/* 仅独立开发探针提供的有界错误注入，正常运行默认未开启。 */
#define RT_MSG_ADC_TEST_FAULT 13U
/* 独立开发诊断只报告 IRQ 分发证据，不产生采样数据。 */
#define RT_MSG_READ_BUSY_PATH 14U
/* 独立诊断读取配置完成后的五片寄存器，不触发转换或产生采样数据。 */
#define RT_MSG_READ_ADC_REGISTERS 15U
/* 只读取已保存的 Timer3/GPIO/BUSY 对照结果，查询本身不驱动引脚。 */
#define RT_MSG_READ_ADC_TIMER_TRIGGER 16U
/* 开发样机有限 CONVST 输出与只读状态；不进入 ADC 采样资格。 */
#define RT_MSG_ADC_SCOPE_CONTROL 17U
#define RT_MSG_ADC_SCOPE_STATUS 18U
/* 开发样机普通任务调整采样读时序；只在STOP且DMA空闲时接受。 */
#define RT_MSG_ADC_LBC_CONFIG 19U
#define RT_MSG_ADC_LBC_STATUS 20U
#define RT_RTC_SOURCE_LINUX   1U
#define RT_MSG_ACK_FLAG       (1U << 0)
#define RT_MSG_EVENT_FLAG     (1U << 1)
#define RT_MSG_CRC_FLAG       (1U << 2)
#define RT_EVENT_FAULT        0x8001U
#define RT_EVENT_TIME         0x8002U
/* R 普通任务向 Linux 发送定长调试日志，不允许 ISR 格式化日志。 */
#define RT_EVENT_DEBUG_LOG    0x8003U
#define RT_REPLY_OK           0
#define RT_REPLY_UNSUPPORTED -1
#define RT_REPLY_BAD_LENGTH  -2
#define RT_REPLY_BAD_EPOCH   -3
#define RT_REPLY_BAD_CONFIG  -4
#define RT_REPLY_BAD_STATE   -5
#define RT_REPLY_BUSY        -6
#define RT_REPLY_BAD_CRC     -7

/* 两条单向消息环；ACK 的 request_id 必须与请求一致。 */
/* 命令/事件环固定记录，request_id 匹配 ACK，session_epoch 拒绝旧会话；有效载荷最多 224 字节。 */
struct rt_message_v1 {
    /* id 指定消息类型，request_id 配对 ACK；会话与代次避免旧命令重放。 */
    rt_u32 id, request_id, session_epoch, generation;
    /* payload_bytes 不得超过下方数组；flags 决定 ACK/事件/CRC 的解释。 */
    rt_u32 payload_bytes, flags;
    rt_s32 status;
    rt_u32 payload_crc32;
    rt_u8 payload[224];
} RT_ALIGN64;

/* HELLO 的能力来自实际端口实现；存在结构定义不等于支持相应硬件功能。 */
/* 握手能力与实际端口上限；声明结构不表示硬件功能已实现或验收。 */
struct rt_hello_v1 {
    rt_u32 abi_version, bytes, offered_features, required_features;
    rt_u32 session_epoch, peer_epoch_seen, record_bytes, max_analog_channels;
    rt_u32 max_samples_per_second, tick_hz, pwm_clock_hz, max_message_bytes;
    rt_u32 reserved[4];
};

/* 首阶段仅建立单调时钟的偏移区间，不代表 RTC/PPS 对时已经实现。 */
/* 主端时钟探测请求，携带主端发送计数与关联 ID，仅建立单调时钟映射。 */
struct rt_clock_probe {
    struct rt_words64 host_send_ticks;
    rt_u32 probe_id, reserved;
};
/* 远端时钟探测回复，保留主发/远收/远发计数及频率，用于估算映射区间而非 PPS 锁定。 */
struct rt_clock_reply {
    struct rt_words64 host_send_ticks, remote_receive_ticks, remote_send_ticks;
    rt_u32 probe_id, remote_tick_hz;
};

/* 软件锚点对应已探测的单调计数，不代表 PPS 捕获或硬件 RTC 锁定。 */
/* 软件 UTC 锚点，captured_ticks 在共享 24 MHz 域，time_epoch 区分校时代次；uncertainty/source 保存来源质量。 */
struct rt_soft_rtc_anchor {
    struct rt_words64 captured_ticks, utc_seconds;
    rt_u32 utc_nanoseconds, time_epoch, uncertainty_ns, source;
};

/* 定值中的数据全为整数；CT/PT 比例、校准和保护定值保留在 Linux。 */
/* 固定 256 字节整数采集定值，generation/CRC 关联原子切换，range_code 按总线顺序；CT/PT 与保护定值留在 Linux。 */
struct rt_acq_config_v1 {
    rt_u32 abi_version, bytes, generation, crc32;
    /* 周期单位为 24 MHz tick；使能掩码和过采样码在两端采用同一版本定义。 */
    rt_u32 initial_period_ticks, enabled_chip_mask, oversampling_code, flags;
    rt_u32 samples_per_cycle, notify_every_samples, frame_budget_ticks, entry_limit_ns;
    rt_u32 sync_mode, holdover_ms, tlv_offset, reserved0;
    /* 每片按现有总线 A0/B0/A1/B1/... 顺序；量程码 1/2/3 的含义由 AD7616 适配层核对。 */
    rt_u8 range_code[RT_ANALOG_CHANNELS];
    rt_u8 reserved[112];
} RT_ALIGN64;
/* 配置银行提交请求，bank_offset 是共享偏移，bytes/generation/crc32 必须与已写银行一致。 */
struct rt_config_request {
    rt_u32 bank_offset, bytes, generation, crc32;
};
/* 远端配置确认，返回生效代次/周期和首样点序号，主端收到后才认定新定值有效。 */
struct rt_apply_result {
    rt_u32 generation, active_period_ticks;
    struct rt_words64 first_sample_sequence;
};
/* 时钟锚点指向指定 PPS 捕获，不以 Linux 消息到达时刻作为秒边沿。 */
/* 指定 PPS 捕获的 UTC 锚点，pps_sequence/captured_ticks 绑定实际边沿，质量/守时/频偏由实现负责。 */
struct rt_time_anchor_v1 {
    rt_u32 abi_version, bytes, pps_sequence, time_epoch;
    struct rt_words64 captured_ticks, utc_seconds;
    rt_u32 utc_nanoseconds, quality, holdover_ms;
    rt_s32 frequency_correction_ppb;
    rt_u32 reserved[4];
};
/* 周期修改请求，generation 区分定值，earliest_sample_sequence 指定最早允许生效的采样序号。 */
struct rt_period_request {
    rt_u32 period_ticks, generation;
    struct rt_words64 earliest_sample_sequence;
};
/* TLV 长度包含本头，按四字节对齐；未知的必需项拒绝整个事务。 */
/* 四字节对齐扩展项，bytes 包含头；未知必需项拒绝事务，未知可选项允许跳过。 */
struct rt_tlv { rt_u16 type, flags; rt_u32 bytes; };
#define RT_TLV_REQUIRED 1U

RT_ASSERT(sizeof(struct rt_words64) == 8, words64);
RT_ASSERT(sizeof(struct rt_region_header) == 64, region_header);
RT_ASSERT(sizeof(struct rt_object) == 32, object);
RT_ASSERT(sizeof(struct rt_ring_control) == 192, ring_control);
RT_ASSERT(offsetof(struct rt_ring_control, producer) == 64, producer);
RT_ASSERT(offsetof(struct rt_ring_control, consumer) == 128, consumer);
RT_ASSERT(sizeof(struct rt_sample_v1) == 256, sample);
RT_ASSERT(offsetof(struct rt_sample_v1, analog) == 64, analog);
RT_ASSERT(offsetof(struct rt_sample_v1, payload_crc32) == 240, sample_crc);
RT_ASSERT(sizeof(struct rt_peer_status) == 128, peer);
RT_ASSERT(sizeof(struct rt_message_v1) == 256, message);
RT_ASSERT(sizeof(struct rt_hello_v1) == 64, hello);
RT_ASSERT(sizeof(struct rt_clock_probe) == 16, clock_probe);
RT_ASSERT(sizeof(struct rt_clock_reply) == 32, clock_reply);
RT_ASSERT(sizeof(struct rt_soft_rtc_anchor) == 32, soft_rtc_anchor);
RT_ASSERT(offsetof(struct rt_message_v1, payload) == 32, payload);
RT_ASSERT(sizeof(struct rt_acq_config_v1) == 256, config);
RT_ASSERT(sizeof(struct rt_time_anchor_v1) == 64, time_anchor);
RT_ASSERT(RT_SAMPLE_DATA_OFF + RT_SAMPLE_CAPACITY * RT_RECORD_BYTES <= RT_REGION_BYTES, fits);
#endif
