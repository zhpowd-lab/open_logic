#ifndef OPEN_LOGIC16_H
#define OPEN_LOGIC16_H
#include "logic16-layout.h"

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
typedef u32 l16_u32;
typedef u16 l16_u16;
#else
#include <stdint.h>
typedef uint32_t l16_u32;
typedef uint16_t l16_u16;
#endif

enum {
    L16_MAGIC = 0x3631474cU, /* 小端共享状态标识 LG16。 */
    L16_VERSION = 3U, /* ABI3：末尾状态字用于独立的当前开入监视。 */
    L16_PROGRESS_SAMPLES = 1024U, /* 每批核验后才发布连续前缀。 */
    L16_PRIMING_READS = 16U, /* 不计入正式段的有界准备事务。 */
    L16_READ_TIMEOUT_TICKS = 480U, /* 单笔 RX 最多等待 20 微秒。 */
    L16_ARM_DELAY_TICKS = 48U, /* 首个正式期限前留 2 微秒，避免准备后长时间空闲。 */
    L16_IDLE_TICKS = 24000U, /* 空闲时每毫秒检查一次新命令。 */
    L16_LIVE_WORD_MASK = 0xffffU, /* 实时读数保持原始总线位序，不反相。 */
    L16_LIVE_VALID = (1U << 16U), /* 仅最新一次总线事务成功才标记有效。 */
    L16_LIVE_ENABLED = (1U << 17U), /* 明确启用后才在空闲期持续读总线。 */
    L16_LIVE_ERROR_SHIFT = 24U, /* 实时监视故障独立于已冻结段的错误。 */
    L16_U32_HIGH_SHIFT = 32U, /* 64 位 tick 的高半字位置。 */
    L16_U32_MAX = 0xffffffffU, /* 未初始化最小间隔的哨兵。 */
    L16_PATTERN_PHASE_MASK = 1023U, /* 测试图案每 1024 点重复脉冲宽度。 */
    L16_PATTERN_WIDE_BEGIN = 128U, /* 256 点宽脉冲起点。 */
    L16_PATTERN_WIDE_END = 384U, /* 宽脉冲终点，不包含。 */
    L16_PATTERN_MEDIUM_BEGIN = 512U, /* 64 点宽脉冲起点。 */
    L16_PATTERN_MEDIUM_END = 576U, /* 中脉冲终点，不包含。 */
    L16_PATTERN_SHORT_BEGIN = 768U, /* 16 点宽脉冲起点。 */
    L16_PATTERN_SHORT_END = 784U, /* 短脉冲终点，不包含。 */
    L16_PATTERN_TINY_BEGIN = 896U, /* 4 点宽脉冲起点。 */
    L16_PATTERN_TINY_END = 900U, /* 窄脉冲终点，不包含。 */
    L16_PATTERN_NOTCH_BEGIN = 500248U, /* 一次缺口的首个原始索引。 */
    L16_PATTERN_NOTCH_END = 500252U, /* 缺口的结束索引，不包含。 */
    L16_PATTERN_COUNTER_SHIFT = 1U, /* 辅助通道每四点改变一次最低位。 */
    L16_PATTERN_COUNTER_MASK = 0x7ffeU, /* D1..14 为计数图案，D15 固定为零。 */
};

enum l16_state { L16_BOOT, L16_IDLE, L16_RUNNING, L16_FULL, L16_STOPPED, L16_FAULT, L16_OFF };
enum l16_command { L16_ARM = 1, L16_MONITOR = 2 };
enum l16_error { L16_OK, L16_BAD_CPU, L16_BAD_CLOCK, L16_BAD_BUS, L16_READ_TIMEOUT,
                 L16_BUS_ERROR, L16_DEADLINE, L16_BAD_REQUEST, L16_UNEXPECTED_IRQ, L16_CPU_OFF_RETURNED };

/* 全部为固定宽度小端字段；host 只写请求及命令代次，ACK/epoch 归 CPU3；不交换 Linux 指针。 */
struct l16_status {
    l16_u32 seq, magic, version, bytes;
    l16_u32 cpu, state, capture_id, period_ticks;
    l16_u32 limit, count, dropped, error;
    l16_u32 first_lo, first_hi, last_lo, last_hi;
    l16_u32 min_interval, max_interval, max_late, max_read;
    l16_u32 last_word, heartbeat, clock_hz, active_pattern;
    l16_u32 command_gen, command_ack, command, request_period;
    l16_u32 request_limit, request_pattern, reserved0, reserved1;
    l16_u32 priming_reads, priming_max_ticks, reserved2, live_status;
};

struct l16_request { l16_u32 period_ticks, sample_limit, pattern, reserved; };

#if defined(__KERNEL__) || defined(L16_LINUX)
#ifndef __KERNEL__
#include <sys/ioctl.h>
#endif
#define L16_CLAIM _IO('L', 1)
#define L16_RELEASE _IO('L', 2)
#define L16_START _IOW('L', 3, struct l16_request)
#define L16_ENABLE_MONITOR _IO('L', 4)
#endif

static inline l16_u32 l16_capacity(l16_u32 period)
{
    l16_u32 count = period ? L16_CLOCK / period : 0;
    return count > L16_MAX_SAMPLES ? L16_MAX_SAMPLES : count;
}

static inline l16_u16 l16_pattern(l16_u32 index)
{
    l16_u32 phase = index & L16_PATTERN_PHASE_MASK;
    l16_u32 pulse = (phase >= L16_PATTERN_WIDE_BEGIN && phase < L16_PATTERN_WIDE_END) ||
        (phase >= L16_PATTERN_MEDIUM_BEGIN && phase < L16_PATTERN_MEDIUM_END) ||
        (phase >= L16_PATTERN_SHORT_BEGIN && phase < L16_PATTERN_SHORT_END) ||
        (phase >= L16_PATTERN_TINY_BEGIN && phase < L16_PATTERN_TINY_END);
    if (index >= L16_PATTERN_NOTCH_BEGIN && index < L16_PATTERN_NOTCH_END) pulse = 0;
    return (l16_u16)(((index >> L16_PATTERN_COUNTER_SHIFT) & L16_PATTERN_COUNTER_MASK) | pulse);
}

#endif
