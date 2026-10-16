#include <string.h>
#include "board.h"
#include "protocol.h"
#include "rt_acquisition_target.h"
#include "service_window.h"
#include "realtime_io.h"

#ifndef AMP_TEST_CHIP_MASK
#define AMP_TEST_CHIP_MASK 1U
#endif
#if AMP_TEST_CHIP_MASK != 1 && AMP_TEST_CHIP_MASK != 2 && AMP_TEST_CHIP_MASK != 4 && \
    AMP_TEST_CHIP_MASK != 8 && AMP_TEST_CHIP_MASK != 16 && AMP_TEST_CHIP_MASK != 31
#error "Only the isolated one-chip and five-chip experiment builds are supported"
#endif

#define GICD 0x03401000U
#define GICC 0x03402000U
#define STOP_SGI 10U
extern rt_u32 amp_psci_cpu_off(void);
extern void amp_memory_enable(void);
extern void amp_memory_disable(void);
static volatile rt_u32 stop_requested;
static rt_u32 unexpected_irq;
static struct amp_board board = {.stop_requested = &stop_requested};
static struct amp_acquisition acq;
static struct rt_ring samples, commands, events;
static struct rt_acq_config_v1 config;
static rt_u32 host_epoch, host_heartbeat;
static int configured, start_attempted, preparing;
#if AMP_REALTIME_IO
static int rtio_prepared;
#endif
/* 本核计时只留最近完整帧及各阶段极值，停采后再输出，避免串口/共享页扰动。 */
enum { PERF_OBSERVE, PERF_POLL, PERF_HOST, PERF_COMMAND, PERF_STATUS, PERF_PUBLISH, PERF_COUNT };
static rt_u32 perf_last[PERF_COUNT], perf_max[PERF_COUNT];
static void perf_record(rt_u32 stage, rt_u64 started)
{
    if (!acq.armed) return;
    rt_u64 elapsed = amp_ticks() - started;
    rt_u32 ticks = elapsed > 0xffffffffU ? 0xffffffffU : (rt_u32)elapsed;
    perf_last[stage] = ticks;
    if (ticks > perf_max[stage]) perf_max[stage] = ticks;
}

__attribute__((section(".acq_status"), used))
volatile struct amp_acq_status amp_status;

static void barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/* 此函数只改变本核I-cache；MMU/D-cache由私有映射管理，不维护Linux共享L2。 */
static void instruction_cache(int enabled)
{
    rt_u32 control, zero = 0;
    __asm__ volatile("dsb sy\nmcr p15, 0, %0, c7, c5, 0\nisb" :: "r"(zero) : "memory");
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
    if (enabled) control |= 1U << 12;
    else control &= ~(1U << 12);
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\nisb" :: "r"(control) : "memory");
}

static volatile rt_u32 *reg32(rt_u32 address)
{
    return (volatile rt_u32 *)(uintptr_t)address;
}

static rt_u32 load(void *context, rt_u32 offset)
{
    rt_u32 value = *(volatile rt_u32 *)((uintptr_t)context + offset);
    barrier();
    return value;
}

static void store(void *context, rt_u32 offset, rt_u32 value)
{
    barrier();
    *(volatile rt_u32 *)((uintptr_t)context + offset) = value;
    barrier();
}

static void copy_from(void *context, void *out, rt_u32 offset, rt_u32 bytes)
{
    rt_u8 *dst = out;
    volatile const rt_u8 *src = (volatile rt_u8 *)((uintptr_t)context + offset);
    /* 共享结构均按4字节对齐；整字复制减少Device DDR的逐字节总线事务。 */
    typedef rt_u32 alias_u32 __attribute__((__may_alias__));
    if (!(((uintptr_t)dst | (uintptr_t)src) & 3U)) {
        while (bytes >= 4U) {
            *(volatile alias_u32 *)dst = *(volatile const alias_u32 *)src;
            dst += 4; src += 4; bytes -= 4;
        }
    }
    while (bytes--)
        *dst++ = *src++;
    barrier();
}

static void copy_to(void *context, rt_u32 offset, const void *in, rt_u32 bytes)
{
    volatile rt_u8 *dst = (volatile rt_u8 *)((uintptr_t)context + offset);
    const rt_u8 *src = in;
    /* 共享结构均按4字节对齐；整字复制减少Device DDR的逐字节总线事务。 */
    typedef rt_u32 alias_u32 __attribute__((__may_alias__));
    if (!(((uintptr_t)dst | (uintptr_t)src) & 3U)) {
        while (bytes >= 4U) {
            *(volatile alias_u32 *)dst = *(volatile const alias_u32 *)src;
            dst += 4; src += 4; bytes -= 4;
        }
    }
    while (bytes--)
        *dst++ = *src++;
    barrier();
}

static const struct rt_io shared = {
    .context = (void *)(uintptr_t)AMP_ACQ_RING_ADDRESS, .mapped_bytes = RT_REGION_BYTES,
    .load_acquire = load, .store_release = store, .copy_from = copy_from, .copy_to = copy_to,
};

static int publish(void *context, const struct rt_sample_v1 *sample)
{
    rt_u64 started = amp_ticks();
    int result;
    (void)context;
    result = rt_ring_push(&samples, sample, sizeof(*sample));
    perf_record(PERF_PUBLISH, started);
    return result;
}

static void status_publish(rt_u32 stopped, rt_u32 smc)
{
    struct rt_peer_status peer = {0};
    rt_u32 seq = load(shared.context, RT_REMOTE_STATUS_OFF);
    amp_status.seq_lock++;
    barrier();
    amp_status.magic = AMP_ACQ_MAGIC;
    amp_status.version = AMP_ACQ_VERSION;
    amp_status.bytes = sizeof(amp_status);
    amp_status.cpu = ADC_AMP_CPU;
    amp_status.state = preparing ? RT_STATE_ARMED : acq.state;
    amp_status.epoch = acq.epoch;
    amp_status.run_generation = acq.run_generation;
    amp_status.period = acq.plan.period_ticks;
    amp_status.enabled_mask = acq.plan.enabled_chip_mask;
    amp_status.qualified = acq.qualified;
    amp_status.falls = acq.falls;
    amp_status.errors = acq.errors;
    amp_status.faults = acq.faults;
    amp_status.first_error = acq.first_error;
    amp_status.missed_cycles = acq.missed_cycles;
    amp_status.read_errors = acq.read_errors;
    amp_status.ring_errors = acq.ring_errors;
    amp_status.busy_timeouts = acq.busy_timeouts;
    amp_status.read_count = acq.read_count;
    amp_status.read_max_ticks = acq.read_max_ticks;
    amp_status.max_poll_ticks = acq.max_poll_ticks;
    amp_status.read_sum_ticks = rt_words_make(acq.read_sum_ticks);
    amp_status.sample_sequence = rt_words_make(acq.sequence);
    amp_status.ticks = rt_words_make(amp_ticks());
    amp_status.stop_requested = stop_requested;
    amp_status.stopped = stopped || (!board.touched && !board.running && !preparing);
    amp_status.smc_result = smc;
    amp_status.unexpected_irq = unexpected_irq;
    {
        volatile struct amp_bus_di_status *di = (volatile struct amp_bus_di_status *)(uintptr_t)
            (AMP_ACQ_STATUS_ADDRESS + AMP_BUS_DI_STATUS_OFFSET);
        di->seq_lock++;
        barrier();
        di->magic = AMP_BUS_DI_MAGIC; di->version = AMP_BUS_DI_VERSION; di->bytes = sizeof(*di);
        di->epoch = acq.epoch; di->run_generation = acq.run_generation;
        di->di_only = AMP_BUS_DI_ONLY; di->target_period_ticks = AMP_BUS_DI_ONLY ? AMP_BUS_DI_PERIOD_TICKS : acq.plan.period_ticks;
        di->raw = acq.di_raw; di->valid = acq.di_valid; di->errors = acq.di_errors; di->changes = acq.di_changes;
        di->last_ticks = acq.di_last_ticks; di->min_ticks = acq.di_min_ticks; di->max_ticks = acq.di_max_ticks;
        di->scans = rt_words_make(acq.di_scans); di->sum_ticks = rt_words_make(acq.di_sum_ticks);
        di->first_scan_ticks = rt_words_make(acq.di_first_scan_ticks); di->last_scan_ticks = rt_words_make(acq.di_last_scan_ticks);
        di->max_interval_ticks = rt_words_make(acq.di_max_interval_ticks); di->missed_periods = rt_words_make(acq.di_missed_periods);
        barrier(); di->seq_lock++; barrier();
    }
    barrier();
    if (acq.errors) {
        /* 仅首错后写诊断扩展，不在正常BUSY轮询中访问共享诊断DDR。 */
        volatile rt_u32 *trace = (volatile rt_u32 *)(uintptr_t)(AMP_ACQ_STATUS_ADDRESS + 128U);
        trace[0] = 0;
        trace[1] = acq.last_observation.phase;
        trace[2] = (rt_u32)acq.last_observation.busy;
        trace[3] = (rt_u32)acq.last_observation.ticks;
        trace[4] = (rt_u32)(acq.last_observation.ticks >> 32);
        trace[5] = acq.before_observation.phase;
        trace[6] = (rt_u32)acq.before_observation.busy;
        trace[7] = (rt_u32)acq.before_observation.ticks;
        trace[8] = (rt_u32)(acq.before_observation.ticks >> 32);
        trace[9] = acq.fail_poll_flags;
        barrier();
        trace[0] = 0x314c4946U;
    }
    if (!acq.armed && !preparing) {
        /* 停止后的完整读路径统计，与旧read_ns局部范围区分；成功和失败端点均保留。 */
        volatile rt_u32 *trace = (volatile rt_u32 *)(uintptr_t)(AMP_ACQ_STATUS_ADDRESS + 168U);
        trace[0] = 0;
        trace[1] = acq.epoch;
        trace[2] = board.phase_calls;
        trace[3] = board.read_phase[0];
        trace[4] = board.read_phase[1];
        trace[5] = board.read_stage;
        trace[6] = board.read_chip;
        trace[7] = (rt_u32)board.frame_result;
        trace[8] = board.frame_last_ticks;
        trace[9] = board.frame_count;
        trace[10] = board.frame_max_ticks;
        trace[11] = (rt_u32)board.frame_sum_ticks;
        trace[12] = (rt_u32)(board.frame_sum_ticks >> 32);
        barrier();
        trace[0] = 0x31445241U;
        trace = (volatile rt_u32 *)(uintptr_t)(AMP_ACQ_STATUS_ADDRESS + 220U);
        trace[0] = 0x31465250U;
        for (rt_u32 i = 0; i < PERF_COUNT; i++) {
            trace[1U + i] = perf_last[i];
            trace[1U + PERF_COUNT + i] = perf_max[i];
        }
        trace[13] = board.start_bound;
        trace = (volatile rt_u32 *)(uintptr_t)(AMP_ACQ_STATUS_ADDRESS + 280U);
        trace[0] = 0x3142534fU;
        trace[1] = amp_observe_failure.reason;
        trace[2] = amp_observe_failure.elapsed;
        trace[3] = amp_observe_failure.phase;
        trace[4] = amp_observe_failure.after;
        trace[5] = amp_observe_failure.period;
    }
    barrier();
    amp_status.seq_lock++;
    barrier();
    peer.bytes = sizeof(peer);
    peer.state = amp_status.state;
    peer.session_epoch = acq.epoch;
    peer.peer_epoch_seen = host_epoch;
    peer.heartbeat = amp_status.ticks.lo;
    peer.fault_bits = acq.faults;
    peer.active_config_generation = acq.plan.generation;
    peer.latest_sample_sequence = rt_words_make(acq.sequence);
    peer.monotonic_tick_hz = peer.pwm_clock_hz = 24000000U;
    peer.time_quality = acq.rtc.bank[acq.rtc.active].time_epoch ? RT_TIME_FREE_RUNNING : RT_TIME_INVALID;
    peer.time_epoch = acq.rtc.bank[acq.rtc.active].time_epoch;
    peer.sample_drop_count = acq.ring_errors;
    peer.reserved[0] = acq.run_generation;
    store(shared.context, RT_REMOTE_STATUS_OFF, seq + 1U);
    copy_to(shared.context, RT_REMOTE_STATUS_OFF + 4U, (const rt_u8 *)&peer + 4U, sizeof(peer) - 4U);
    store(shared.context, RT_REMOTE_STATUS_OFF, seq + 2U);
}

static int bind_ring(rt_u32 id, struct rt_ring *ring)
{
    struct rt_object object;
    int result = rt_find_object(&shared, id, RT_FEATURES_V1, &object);
    return result ? result : rt_ring_bind(&shared, &object, ring);
}

/* Linux写自身状态页，epoch必须与HELLO一致；缓存旧心跳不能无限延长资源租约。 */
static void host_poll(void)
{
    /* 租约只用固定前5个字；同一seqlock下读取所需字段，避免搬完整128字节页。 */
    rt_u32 host[5];
    rt_u32 first = load(shared.context, RT_HOST_STATUS_OFF);
    if (first & 1U)
        return;
    copy_from(shared.context, host, RT_HOST_STATUS_OFF, sizeof(host));
    if (first != load(shared.context, RT_HOST_STATUS_OFF) || host[0] != first ||
        host[1] != sizeof(struct rt_peer_status) || host[3] != host_epoch || !host_epoch)
        return;
    if (host[4] != host_heartbeat) {
        host_heartbeat = host[4];
        amp_acquisition_host_alive(&acq, amp_ticks());
    }
}

static void command_poll(void)
{
    struct rt_message_v1 request, reply = {0};
    int result = rt_ring_pop(&commands, &request, sizeof(request));
    if (result == RT_EMPTY)
        return;
    if (result) {
        stop_requested = 1;
        return;
    }
    reply.id = request.id;
    reply.request_id = request.request_id;
    reply.session_epoch = acq.epoch;
    reply.flags = RT_MSG_ACK_FLAG;
    reply.status = RT_REPLY_UNSUPPORTED;
    if (request.payload_bytes > sizeof(request.payload) ||
        request.flags != RT_MSG_CRC_FLAG ||
        rt_crc32(request.payload, request.payload_bytes) != request.payload_crc32) {
        reply.status = RT_REPLY_BAD_CRC;
    } else if (request.id == RT_MSG_HELLO && request.payload_bytes == sizeof(host_epoch) &&
               !acq.armed && !host_epoch) {
        memcpy(&host_epoch, request.payload, sizeof(host_epoch));
        reply.status = host_epoch ? RT_REPLY_OK : RT_REPLY_BAD_EPOCH;
    } else if (request.id == RT_MSG_STOP && request.session_epoch == acq.epoch && !request.payload_bytes) {
        /* 未HELLO也可撤源；STOP后永久拒绝START，防止释放资源后的旧映射重新触发。 */
        start_attempted = 1;
        reply.status = amp_acquisition_stop(&acq);
#if AMP_REALTIME_IO
        if (!reply.status) reply.status = amp_rtio_stop();
        amp_rtio_publish(acq.run_generation);
#endif
    } else if (request.session_epoch != acq.epoch || !host_epoch) {
        reply.status = RT_REPLY_BAD_EPOCH;
    } else if (request.id == RT_MSG_APPLY_CONFIG &&
               request.payload_bytes == sizeof(struct rt_config_request) &&
               acq.state == RT_STATE_STOPPED) {
        struct rt_config_request selected;
        struct rt_acq_config_v1 candidate;
        struct rt_adc_register_plan plan;
        memcpy(&selected, request.payload, sizeof(selected));
        /* Linux只传固定bank偏移；本核映射到同一DDR，不能解引用Linux虚拟指针。 */
        if ((selected.bank_offset != RT_CONFIG0_OFF && selected.bank_offset != RT_CONFIG1_OFF) ||
            selected.bytes != sizeof(candidate)) {
            reply.status = RT_REPLY_BAD_CONFIG;
        } else {
            copy_from(shared.context, &candidate, selected.bank_offset, sizeof(candidate));
            reply.status = rt_adc_compile_config(&candidate, &plan);
            if (!reply.status && (candidate.generation != selected.generation ||
                                  candidate.crc32 != selected.crc32))
                reply.status = RT_REPLY_BAD_CRC;
            if (!reply.status && candidate.enabled_chip_mask == AMP_TEST_CHIP_MASK) {
                config = candidate;
                configured = 1;
            } else if (!reply.status) {
                reply.status = RT_REPLY_BAD_CONFIG;
            }
        }
        if (!configured && !reply.status)
            reply.status = RT_REPLY_BAD_CONFIG;
#if AMP_REALTIME_IO
    } else if (request.id == AMP_MSG_PREPARE_RTIO && configured && !start_attempted && !rtio_prepared &&
               request.payload_bytes == sizeof(struct amp_start_request)) {
        struct amp_start_request claim;
        memcpy(&claim,request.payload,sizeof(claim));
        if(claim.version!=AMP_ACQ_VERSION || claim.bytes!=sizeof(claim) ||
           claim.owner_flags!=AMP_OWNER_REQUIRED || claim.host_epoch!=host_epoch)
            reply.status=AMP_EOWNER;
        else {
            reply.status=amp_rtio_start();
            if(!reply.status) rtio_prepared=1;
            amp_rtio_publish(0);
        }
#endif
    } else if (request.id == RT_MSG_SET_SOFT_RTC && request.payload_bytes == sizeof(struct rt_soft_rtc_anchor) &&
               !acq.armed) {
        struct rt_soft_rtc_anchor anchor;
        memcpy(&anchor, request.payload, sizeof(anchor));
        reply.status = rt_soft_rtc_set(&acq.rtc, &anchor, amp_ticks());
    } else if (request.id == RT_MSG_START && configured && !start_attempted &&
               request.payload_bytes == sizeof(struct amp_start_request)) {
        struct amp_start_request claim;
        memcpy(&claim, request.payload, sizeof(claim));
        if (claim.version != AMP_ACQ_VERSION || claim.bytes != sizeof(claim) ||
            claim.owner_flags != AMP_OWNER_REQUIRED || claim.host_epoch != host_epoch)
            reply.status = AMP_EOWNER;
        else {
            /* 每次固件启动最多一次ARM；避免旧命令在STOP后重放并再次触发硬件。 */
            start_attempted = 1;
            preparing = 1;
            status_publish(0, 0);
#if AMP_REALTIME_IO
            reply.status = rtio_prepared ? amp_acquisition_arm(&acq, &config, amp_ticks()) : AMP_EOWNER;
            if (reply.status) (void)amp_rtio_stop();
            amp_rtio_publish(acq.run_generation);
#else
            reply.status = amp_acquisition_arm(&acq, &config, amp_ticks());
#endif
            preparing = 0;
        }
    } else if (request.id == RT_MSG_READ_STATUS && !request.payload_bytes) {
        reply.status = RT_REPLY_OK;
    }
    /* 命令确认满环也撤源，不在实时核等待Linux归还ACK。 */
    status_publish(0, 0);
    if (rt_ring_push(&events, &reply, sizeof(reply)))
        stop_requested = 1;
}

void amp_irq(void)
{
    rt_u32 token = *reg32(GICC + 0xcU), irq = token & 1023U;
    if (irq >= 1020U)
        return;
#if AMP_REALTIME_IO
    if (irq == AMP_RTIO_IRQ) {
        amp_rtio_irq(amp_ticks());
        *reg32(GICC + 0x10U) = token;
        barrier();
        return;
    }
#endif
    if (irq != STOP_SGI) {
        unexpected_irq++;
        *reg32(GICC) = 0;
    }
    stop_requested = 1;
    *reg32(GICC + 0x10U) = token;
    barrier();
}

void amp_main(void)
{
    struct amp_acquisition_ops ops = {
        .context = &board, .prepare = amp_board_prepare, .start = amp_board_start,
        .stop = amp_board_stop, .read = amp_board_read, .publish = publish,
        .read_digital = amp_board_read_digital,
    };
    rt_u32 cpu, hz, epoch, result;
    rt_u64 last_status, last_command;
#if AMP_REALTIME_IO
    rt_u64 last_aux = 0;
#endif
    rt_u32 serviced_read_count = 0;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(cpu));
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(hz));
    if ((cpu & 255U) != ADC_AMP_CPU || hz != 24000000U)
        for (;;) { __asm__ volatile("wfi"); }
    amp_memory_enable();
    instruction_cache(1);
    epoch = (rt_u32)amp_ticks() | 1U;
    amp_acquisition_init(&acq, &ops, epoch);
#if AMP_REALTIME_IO
    amp_rtio_init(epoch);
    amp_rtio_publish(0);
#endif
    memset((void *)&amp_status, 0, sizeof(amp_status));
    memset((void *)(uintptr_t)(AMP_ACQ_STATUS_ADDRESS + AMP_BUS_DI_STATUS_OFFSET), 0, sizeof(struct amp_bus_di_status));
    /* 首错扩展只属于本次固件实例，不能残留上一次epoch的失败。 */
    memset((void *)(uintptr_t)(AMP_ACQ_STATUS_ADDRESS + 128U), 0, 40U);
    /* 专用预留区由本次A7启动独占初始化；旧Linux消费者必须先退出。 */
    result = rt_region_format(&shared, epoch, RT_FEAT_RAW80 | RT_FEAT_CONFIG | RT_FEAT_TIME | RT_FEAT_PAYLOAD_CRC);
    if (result || bind_ring(RT_OBJ_SAMPLE, &samples) || bind_ring(RT_OBJ_COMMAND, &commands) || bind_ring(RT_OBJ_EVENT, &events))
        for (;;) { __asm__ volatile("wfi"); }
    *reg32(GICC) = 0;
    *reg32(GICD + 0x180U) = 0xffff0000U;
    *reg32(GICD + 0x80U) |= 1U << STOP_SGI;
    *(volatile rt_u8 *)(uintptr_t)(GICD + 0x400U + STOP_SGI) = 0x80U;
    *reg32(GICD + 0x100U) = 1U << STOP_SGI;
    *reg32(GICC + 4U) = 0xffU;
    *reg32(GICC + 8U) = 0;
    *reg32(GICC) = 1;
    last_status = last_command = amp_ticks();
    status_publish(0, 0);
    __asm__ volatile("cpsie i" ::: "memory");
    for (;;) {
        struct amp_observation now;
        rt_u64 tick;
        int serviced_command = 0;
        if (stop_requested) {
            /* 严格先停PWM并恢复引脚，成功后才允许PSCI归还CPU；失败留现场。 */
            result = (rt_u32)amp_acquisition_stop(&acq);
#if AMP_REALTIME_IO
            if (!result) result = (rt_u32)amp_rtio_stop();
            amp_rtio_publish(acq.run_generation);
#endif
            status_publish(!result, 0);
            if (!result) {
                __asm__ volatile("cpsid if" ::: "memory");
                *reg32(GICD + 0x180U) = 1U << STOP_SGI;
                amp_memory_disable();
                instruction_cache(0);
                result = amp_psci_cpu_off();
                status_publish(1, result);
            }
            for (;;) { __asm__ volatile("wfi"); }
        }
        if (acq.armed && AMP_BUS_DI_ONLY) {
            now = (struct amp_observation){.ticks = amp_ticks()};
            amp_acquisition_poll(&acq, &now);
        } else if (acq.armed) {
            rt_u64 started = amp_ticks();
            rt_u32 count = acq.read_count;
            int observation_result = amp_board_observe(&board, &now);
            if (observation_result == AMP_OBSERVE_RETRY && acq.synchronized &&
                amp_ticks() >= acq.previous.ticks &&
                amp_ticks() - acq.previous.ticks < acq.plan.period_ticks) {
                /* 不一致快照不上交状态机、不处理后台事务。下一次完整快照
                 * 仍检查原始elapsed/BUSY/读窗，绝不插补或跨周期放行。 */
                continue;
            }
            if (observation_result) {
                /* 由状态机走统一故障撤源，不把寄存器读取失败当正常低BUSY。 */
                now.busy = -1;
            }
            rt_u32 observed = (rt_u32)(amp_ticks() - started);
            started = amp_ticks();
            amp_acquisition_poll(&acq, &now);
            if (acq.read_count != count) {
                perf_last[PERF_OBSERVE] = observed;
                if (observed > perf_max[PERF_OBSERVE]) perf_max[PERF_OBSERVE] = observed;
                perf_record(PERF_POLL, started);
                perf_last[PERF_HOST] = perf_last[PERF_COMMAND] = perf_last[PERF_STATUS] = 0;
            }
        }
        tick = amp_ticks();
        /* 大块共享页与命令处理放在本周期读完之后，避免错过窄BUSY高电平。 */
        /* 每次读完立即处理一次后台事务；禁止在周期尾部再次进入长共享页操作，
         * 否则处理过程可能覆盖下一次BUSY高电平，造成真实转换漏观测。 */
        if (!acq.armed || AMP_BUS_DI_ONLY || (acq.completed && acq.read_count != serviced_read_count)) {
            serviced_read_count = acq.read_count;
            /* 固定DI周期尾部只等待采样，避免共享状态发布跨过下一读时刻。
             * 与ADC一样保留8us后台余量；过高频率无法服务租约时显式失败。 */
            if (acq.armed && AMP_BUS_DI_ONLY &&
                (tick >= acq.di_due_ticks || acq.di_due_ticks - tick <= 192U))
                continue;
            /* 计时表已观察到读后处理变长；此时不能再叠加后台DDR事务。
             * 8us保留覆盖实测后台最大5.25us及下一次观测，来不及则让下一完整帧处理。 */
            if (acq.armed && !AMP_BUS_DI_ONLY && !amp_service_window(acq.plan.period_ticks, now.phase,
                                                  now.ticks, amp_ticks(), 192U))
                continue;
            /* 状态到期优先独占一帧。低采样率每帧都超过命令间隔，若先处理
             * 命令再以serviced_command排除状态，会永久饿死原生状态发布。 */
            int status_due = tick - last_status >= 24000U;
#if AMP_REALTIME_IO
            /* 辅助快照与原大状态/命令分帧发布；I2C每次仅推进一步，仍受原8us余量门控。 */
            if (!status_due && tick - last_aux >= 240000U) {
                amp_rtio_publish(acq.run_generation);
                last_aux = amp_ticks();
                continue;
            }
#endif
            if (tick - last_command >= 2400U && (!acq.armed || !status_due)) {
                rt_u64 started = amp_ticks();
                int was_armed = acq.armed;
                host_poll();
                perf_record(PERF_HOST, started);
                started = amp_ticks();
                command_poll();
                if (was_armed) perf_record(PERF_COMMAND, started);
                last_command = amp_ticks();
                serviced_command = 1;
            }
            /* 命令/租约与大状态页分到不同完整帧，不能叠加遮住下一次BUSY。 */
            /* 停止态也限频，否则持续写seqlock会让Linux无法取得一致快照。
             * 命令ACK另有即时发布，首错最迟在下个1ms诊断时刻可见。 */
            if (tick - last_status >= 24000U && (!acq.armed || !serviced_command)) {
                rt_u64 started = amp_ticks();
                status_publish(0, 0);
                perf_record(PERF_STATUS, started);
                last_status = amp_ticks();
#if AMP_REALTIME_IO
            } else if (!serviced_command) {
                amp_rtio_service(amp_ticks());
#endif
            }
        }
    }
}
