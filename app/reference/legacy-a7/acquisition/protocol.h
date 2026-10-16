#ifndef AMP_ACQUISITION_PROTOCOL_H
#define AMP_ACQUISITION_PROTOCOL_H
#include "rt_shared_abi.h"
#include "bus_di.h"

#define AMP_ACQ_STATUS_ADDRESS 0x442f0000U
#define AMP_ACQ_RING_ADDRESS 0x44300000U
#define AMP_ACQ_MAGIC 0x31435141U
/* v2要求共享DDR双方均为Normal Non-cacheable；拒绝与旧Device映射配套混用。 */
#define AMP_ACQ_VERSION 2U
/* 只用于隔离样机资源交接声明，不是安全认证，也不能替代Linux原生资源持有。 */
#define AMP_OWNER_E907_OFFLINE (1U << 0)
#define AMP_OWNER_LINUX_RESERVED (1U << 1)
#define AMP_OWNER_CLOCKS_HELD (1U << 2)
#define AMP_OWNER_OUTPUTS_BLOCKED (1U << 3)
#define AMP_OWNER_REQUIRED 15U
/* 先取得辅助采集，允许从A7 RTC恢复Linux UTC；此命令绝不触发ADC。 */
#define AMP_MSG_PREPARE_RTIO 0x4137494fU
struct amp_start_request {
    rt_u32 version, bytes, owner_flags, host_epoch;
};

/* 独立诊断页固定地址及宽度；只有A7写，Linux用seq_lock读取完整快照。 */
struct amp_acq_status {
    rt_u32 magic, version, bytes, seq_lock;
    rt_u32 cpu, state, epoch, run_generation;
    rt_u32 period, enabled_mask, qualified, falls;
    rt_u32 errors, faults;
    rt_s32 first_error;
    rt_u32 missed_cycles, read_errors, ring_errors, busy_timeouts;
    rt_u32 read_count, read_max_ticks, max_poll_ticks;
    struct rt_words64 read_sum_ticks, sample_sequence, ticks;
    rt_u32 stop_requested, stopped, smc_result, unexpected_irq;
};
RT_ASSERT(sizeof(struct amp_acq_status) == 128, amp_acq_status_size);
RT_ASSERT(sizeof(struct amp_start_request) == 16, amp_start_request_size);
#endif
