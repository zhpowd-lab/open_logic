#include "platform.h"

enum {
    GICD = 0x03401000U, /* 只访问本核 banked 的 SGI/PPI 配置。 */
    GICC = 0x03402000U, /* CPU3 的中断接口。 */
    GICC_ACK = 0xcU, /* 读取当前中断 token。 */
    GICC_EOI = 0x10U, /* 完成当前 token。 */
    GICC_PRIORITY = 4U, /* 本核优先级屏蔽。 */
    GICC_BINARY_POINT = 8U, /* 本核抢占优先级分组。 */
    GICC_ALLOW_ALL = 0xffU, /* 放行本项目的 SGI。 */
    GICD_DISABLE = 0x180U, /* banked 中断关闭寄存器。 */
    GICD_GROUP = 0x80U, /* banked 中断分组。 */
    GICD_PRIORITY = 0x400U, /* 每个中断的优先级字节。 */
    GICD_ENABLE = 0x100U, /* banked 中断开启寄存器。 */
    GIC_PPI_MASK = 0xffff0000U, /* 只关闭本核 PPI，不改 SPI 分发器。 */
    GIC_IRQ_MASK = 1023U, /* GIC token 的 IRQ 字段。 */
    GIC_SPURIOUS_FIRST = 1020U, /* 特殊/伪中断 token 的起点。 */
    L16_STOP_SGI = 10U, /* 当前 A7 RemoteProc 的关闭 SGI。 */
    L16_STOP_MASK = (1U << L16_STOP_SGI), /* 只开启此 SGI。 */
    L16_STOP_PRIORITY = 0x80U, /* 关闭 SGI 的优先级。 */
    L16_CPU_ID_MASK = 0xffU, /* MPIDR 的本核编号字段。 */
};
#define L16_MEMBER_WORD(member) (__builtin_offsetof(struct l16_status, member) / sizeof(uint32_t))
__attribute__((section(".status"), used)) volatile struct l16_status l16_status;
static volatile uint32_t shutdown_requested;
static struct l16_status local;
/* 无 RPMsg/业务端点；空表用于匹配内核 RemoteProc 的标准装载合同。 */
__attribute__((section(".resource_table"), used, aligned(8)))
static const uint32_t resource_table[] = {
    1, 0, 0, 0
};
__attribute__((section(".version_table"), used))
static const char firmware_version[] = "open_logic16 ABI3";

static void publish(void)
{
    uint32_t i;
    volatile uint32_t *target = (volatile uint32_t *)&l16_status;
    const uint32_t *source = (const uint32_t *)&local;
    l16_status.seq++;
    l16_barrier();
    /* 命令区只归 Linux 写；命令 ACK 由 CPU3 单独发布。 */
    for (i = L16_MEMBER_WORD(magic); i < L16_MEMBER_WORD(command_gen); i++) target[i] = source[i];
    for (i = L16_MEMBER_WORD(priming_reads); i < L16_STATUS_WORDS; i++) target[i] = source[i];
    l16_barrier();
    l16_status.seq++;
}

#ifndef L16_HOST_TEST
void amp_irq(void)
{
    uint32_t token = l16_read32(GICC + GICC_ACK), irq = token & GIC_IRQ_MASK;
    if (irq >= GIC_SPURIOUS_FIRST) return;
    if (irq != L16_STOP_SGI) {
        local.error = L16_UNEXPECTED_IRQ;
        l16_write32(GICC, 0);
    }
    shutdown_requested = 1;
    l16_write32(GICC + GICC_EOI, token);
    l16_barrier();
}
#endif

struct capture_progress {
    uint64_t origin;
    uint32_t due, count, first_low, previous;
    uint32_t committed, committed_last, warming, error;
};

static void reset_window_stats(void)
{
    local.count = 0;
    local.dropped = 0;
    local.error = 0;
    local.last_word = 0;
    local.first_lo = 0;
    local.first_hi = 0;
    local.last_lo = 0;
    local.last_hi = 0;
    local.min_interval = L16_U32_MAX;
    local.max_interval = 0;
    local.max_late = 0;
    local.max_read = 0;
}

static uint32_t begin_capture(uint32_t period, uint32_t limit, uint32_t pattern)
{
    uint32_t error = pattern ? L16_OK : l16_board_prepare();
    local.state = error ? L16_FAULT : L16_RUNNING;
    local.capture_id++;
    local.period_ticks = period;
    local.limit = limit;
    local.active_pattern = pattern;
    reset_window_stats();
    local.priming_reads = 0;
    local.priming_max_ticks = 0;
    local.error = error;
    publish();
    /* 先公布 running/fault 再 ACK，避免 Linux 在旧 full 状态下误收第二次 ARM。 */
    l16_status.command_ack = l16_status.command_gen;
    return error;
}

static void finish_capture(struct capture_progress *progress, uint32_t pattern)
{
    volatile uint16_t *samples = (volatile uint16_t *)(uintptr_t)L16_SAMPLE_ADDRESS;
    uint64_t first = 0;
    uint64_t last = 0;
    if (progress->warming) progress->count = 0;
    if (!pattern) {
        uint32_t checked = l16_board_check();
        if (checked && !progress->error) progress->error = checked;
        if (checked || progress->error == L16_BUS_ERROR || progress->error == L16_READ_TIMEOUT) {
            /* 粘滞错误无法定位到批内哪一字，只保留之前已核验的连续前缀。 */
            progress->count = progress->committed;
            progress->previous = progress->committed_last;
        }
    }
    local.last_word = progress->count ? samples[progress->count - 1U] : 0;
    if (progress->count) {
        first = progress->origin + (uint32_t)(progress->first_low - (uint32_t)progress->origin);
        last = progress->origin + (uint32_t)(progress->previous - (uint32_t)progress->origin);
    }
    local.count = progress->count;
    local.first_lo = (uint32_t)first;
    local.first_hi = (uint32_t)(first >> L16_U32_HIGH_SHIFT);
    local.last_lo = (uint32_t)last;
    local.last_hi = (uint32_t)(last >> L16_U32_HIGH_SHIFT);
    if (progress->count <= 1U) local.min_interval = 0;
    if (shutdown_requested && local.error == L16_UNEXPECTED_IRQ) progress->error = local.error;
    local.error = progress->error;
    local.state = local.error ? L16_FAULT : (progress->count == local.limit ? L16_FULL : L16_STOPPED);
    l16_barrier();
    publish();
}

__attribute__((noinline, flatten)) static void acquire(uint32_t period, uint32_t limit, uint32_t pattern)
{
    volatile uint16_t *samples = (volatile uint16_t *)(uintptr_t)L16_SAMPLE_ADDRESS;
    struct capture_progress progress = {
        0
    };
    if (begin_capture(period, limit, pattern)) return;
    progress.warming = !pattern;
    progress.origin = l16_ticks();
    progress.due = (uint32_t)progress.origin + L16_ARM_DELAY_TICKS;
    /* 低 32 位差值覆盖不到半圈，单段最多 1 秒；首尾 64 位时标在结束后恢复。 */
    while ((progress.warming || progress.count < limit) && !shutdown_requested) {
        uint16_t word;
        uint32_t now, completed, elapsed, late;
        do {
            now = (uint32_t)l16_ticks();
        } while ((int32_t)(now - progress.due) < 0 && !shutdown_requested);
        if (shutdown_requested) break;
        late = now - progress.due;
        if (!progress.warming && late > local.max_late) local.max_late = late;
        if (!progress.warming && late >= period) {
            local.dropped = late / period;
            progress.error = L16_DEADLINE;
            break;
        }
        if (pattern) word = l16_pattern(progress.count);
        else {
            progress.error = l16_board_read(&word, &shutdown_requested);
            if (progress.error) break;
        }
        completed = (uint32_t)l16_ticks();
        elapsed = completed - now;
        if (progress.warming) {
            local.priming_reads++;
            if (elapsed > local.priming_max_ticks) local.priming_max_ticks = elapsed;
            progress.due = now;
        } else {
            if (elapsed > local.max_read) local.max_read = elapsed;
            if (completed - progress.due >= period) {
                local.dropped = (completed - progress.due) / period;
                progress.error = L16_DEADLINE;
                break;
            }
        }
        /* 准备阶段也走实际存储和统计路径，但不发布；正式段从索引零覆盖它们。 */
        samples[progress.count] = word;
        if (!progress.count) progress.first_low = now;
        else {
            uint32_t interval = now - progress.previous;
            if (interval < local.min_interval) local.min_interval = interval;
            if (interval > local.max_interval) local.max_interval = interval;
        }
        progress.previous = now;
        local.last_word = word;
        progress.count++;
        if (progress.warming && progress.count == L16_PRIMING_READS) {
            progress.error = l16_board_check();
            if (progress.error) break;
            progress.warming = 0;
            progress.count = 0;
            reset_window_stats();
            progress.origin = l16_ticks();
            progress.due = (uint32_t)progress.origin + period;
            continue;
        }
        if (!progress.warming && !(progress.count & (L16_PROGRESS_SAMPLES - 1U))) {
            if (!pattern) {
                progress.error = l16_board_check();
                if (progress.error) break;
            }
            l16_barrier();
            l16_status.count = progress.count;
            progress.committed = progress.count;
            progress.committed_last = progress.previous;
        }
        progress.due += period;
    }
    finish_capture(&progress, pattern);
}

static void enable_monitor(void)
{
    uint32_t error = l16_board_prepare();
    if (!error) local.reserved2 = L16_LIVE_VALID | l16_board_probe_inputs();
    local.live_status = error ? error << L16_LIVE_ERROR_SHIFT : L16_LIVE_ENABLED;
    publish();
    l16_status.command_ack = l16_status.command_gen;
}

static void monitor_once(void)
{
    uint16_t word;
    uint32_t error;
    if (!(local.live_status & L16_LIVE_ENABLED)) return;
    error = l16_board_read(&word, &shutdown_requested);
    if (!error) error = l16_board_check();
    /* 监视故障不篡改波形段，也不在总线异常时无限重试。 */
    local.live_status = error ? error << L16_LIVE_ERROR_SHIFT :
        L16_LIVE_ENABLED | L16_LIVE_VALID | word;
    local.heartbeat++;
    publish();
}

#ifndef L16_HOST_TEST
void amp_main(void)
{
    uint32_t cpu, hz;
    uint64_t idle_due;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(hz));
    l16_memory_enable();
    local.magic = L16_MAGIC;
    local.version = L16_VERSION;
    local.bytes = sizeof(local);
    local.cpu = cpu & L16_CPU_ID_MASK;
    local.clock_hz = hz;
    local.state = L16_IDLE;
    if (local.cpu != L16_CPU) {
        local.state = L16_FAULT;
        local.error = L16_BAD_CPU;
    }
    if (hz != L16_CLOCK) {
        local.state = L16_FAULT;
        local.error = L16_BAD_CLOCK;
    }
    l16_status.seq = l16_status.command_gen = l16_status.command_ack = 0;
    idle_due = l16_ticks();
    l16_status.reserved0 = (uint32_t)idle_due;
    l16_status.reserved1 = (uint32_t)(idle_due >> L16_U32_HIGH_SHIFT);
    publish();
    /* 只操作本核 banked 的 SGI/PPI，保留 Linux 的分配器和共享中断配置。 */
    l16_write32(GICC, 0);
    l16_write32(GICD + GICD_DISABLE, GIC_PPI_MASK);
    l16_write32(GICD + GICD_GROUP, l16_read32(GICD + GICD_GROUP) | L16_STOP_MASK);
    *(volatile uint8_t *)(uintptr_t)(GICD + GICD_PRIORITY + L16_STOP_SGI) = L16_STOP_PRIORITY;
    l16_write32(GICD + GICD_ENABLE, L16_STOP_MASK);
    l16_write32(GICC + GICC_PRIORITY, GICC_ALLOW_ALL);
    l16_write32(GICC + GICC_BINARY_POINT, 0);
    l16_write32(GICC, 1);
    l16_barrier();
    __asm__ volatile("cpsie i" ::: "memory");
    idle_due = l16_ticks();
    while (!shutdown_requested) {
        uint32_t generation;
        if (l16_ticks() < idle_due) continue;
        idle_due = l16_ticks() + L16_IDLE_TICKS;
        generation = l16_status.command_gen;
        l16_barrier();
        if (generation != l16_status.command_ack) {
            uint32_t period = l16_status.request_period, limit = l16_status.request_limit;
            uint32_t pattern = l16_status.request_pattern, command = l16_status.command;
            if (command == L16_MONITOR && local.cpu == L16_CPU && hz == L16_CLOCK)
                enable_monitor();
            else if (command == L16_ARM && period >= L16_MIN_PERIOD && period <= L16_CLOCK &&
                limit && limit <= l16_capacity(period) && pattern <= 1 && local.cpu == L16_CPU && hz == L16_CLOCK)
                acquire(period, limit, pattern);
            else {
                local.state = L16_FAULT;
                local.error = L16_BAD_REQUEST;
                publish();
                l16_status.command_ack = generation;
            }
        }
        /* 当前值独立于样本区；满深度后继续监视，不循环覆盖波形段。 */
        monitor_once();
    }
    if (!l16_board_restore()) {
        local.state = L16_OFF;
        publish();
        __asm__ volatile("cpsid if" ::: "memory");
        l16_write32(GICD + GICD_DISABLE, L16_STOP_MASK);
        l16_memory_disable();
        /* PSCI 成功不会返回；任何返回都属于关闭失败，保持故障信息供 Linux 检查。 */
        local.reserved2 = amp_psci_cpu_off();
        local.error = L16_CPU_OFF_RETURNED;
        local.state = L16_FAULT;
        publish();
    } else {
        local.state = L16_FAULT;
        local.error = L16_BAD_BUS;
        publish();
    }
    for (;;) __asm__ volatile("wfi");
}
#endif
