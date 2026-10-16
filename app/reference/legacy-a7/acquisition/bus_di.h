#ifndef AMP_BUS_DI_H
#define AMP_BUS_DI_H

/* 每帧只读取片选5的一组16位开入；独立定频固件绝不触发/读取ADC。 */
#ifndef AMP_BUS_DI_ONLY
#define AMP_BUS_DI_ONLY 0
#endif
#ifndef AMP_BUS_DI_PERIOD_TICKS
#define AMP_BUS_DI_PERIOD_TICKS (AMP_BUS_DI_ONLY ? 2400U : 0U)
#endif
#if AMP_BUS_DI_ONLY != 0 && AMP_BUS_DI_ONLY != 1
#error "AMP_BUS_DI_ONLY must be 0 or 1"
#endif
#if AMP_BUS_DI_PERIOD_TICKS > 24000000U
#error "DI test period exceeds one second"
#endif
#if AMP_BUS_DI_ONLY && AMP_BUS_DI_PERIOD_TICKS == 0
#error "DI logic acquisition requires a fixed nonzero period"
#endif

#define AMP_BUS_DI_CHIP 5U
#define AMP_BUS_DI_VALID 0xffffU

#define AMP_BUS_DI_STATUS_OFFSET 320U
#define AMP_BUS_DI_MAGIC 0x31494442U
#define AMP_BUS_DI_VERSION 1U
/* 与128字节原生状态分开演进；地址7是板址，不属于有效开入。 */
struct amp_bus_di_status {
    rt_u32 magic, version, bytes, seq_lock;
    rt_u32 epoch, run_generation, di_only, target_period_ticks;
    rt_u32 raw, valid, errors, changes;
    rt_u32 last_ticks, min_ticks, max_ticks, reserved;
    struct rt_words64 scans, sum_ticks, first_scan_ticks, last_scan_ticks;
    struct rt_words64 max_interval_ticks, missed_periods;
};
RT_ASSERT(sizeof(struct amp_bus_di_status) == 112, amp_bus_di_status_size);
#endif
