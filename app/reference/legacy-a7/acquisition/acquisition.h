#ifndef AMP_ACQUISITION_H
#define AMP_ACQUISITION_H

#include "adc_config.h"
#include "rt_software_clock.h"
#include "sample_time.h"
#include "bus_di.h"

/* 本地状态机，不进入共享 ABI；首版仅接受显式的 IDA+PIO。 */
#define AMP_QUALIFY_FRAMES 128U
#define AMP_POLL_GUARD_TICKS 96U
#define AMP_HOST_LEASE_TICKS 24000000U
#define AMP_EBUSY_EDGE (-100)
#define AMP_EMISSED_CYCLE (-101)
#define AMP_EHOST_LEASE (-102)
#define AMP_ECLOCK (-103)
#define AMP_EOWNER (-104)

struct amp_observation {
    rt_u64 ticks;
    rt_u32 phase, period;
    int busy;
};

struct amp_acquisition_ops {
    void *context;
    int (*prepare)(void *context, const struct rt_adc_register_plan *plan);
    int (*start)(void *context, rt_u32 period);
    int (*stop)(void *context);
    int (*read)(void *context, const struct rt_adc_busy_event *event,
                rt_s16 analog[RT_ANALOG_CHANNELS], rt_u32 *mask, rt_u64 *read_ns);
    int (*publish)(void *context, const struct rt_sample_v1 *sample);
    int (*read_digital)(void *context, rt_u32 *raw, rt_u32 *duration_ticks);
};

struct amp_acquisition {
    struct amp_acquisition_ops ops;
    struct rt_adc_register_plan plan;
    struct rt_soft_rtc rtc;
    struct amp_sample_time sample_time;
    /* 私有完整帧在ARM前清零，运行时只更新本帧字段，避免BUSY后重复清256字节。 */
    struct rt_sample_v1 sample;
    struct amp_observation previous, last_observation, before_observation;
    rt_u32 fail_poll_flags;
    rt_u32 epoch, state, qualified, run_generation, errors, faults;
    rt_u32 falls, missed_cycles, read_errors, ring_errors, busy_timeouts;
    rt_u32 max_poll_ticks, read_count, read_max_ticks;
    rt_u64 read_sum_ticks, sequence, last_sample_ticks;
    rt_u64 host_seen_ticks;
    rt_u64 phase_progress_ticks;
    int synchronized, seen_high, completed, armed;
    int first_error;
    rt_u32 di_raw, di_valid, di_errors, di_changes;
    rt_u32 di_last_ticks, di_min_ticks, di_max_ticks;
    rt_u64 di_scans, di_sum_ticks, di_first_scan_ticks, di_last_scan_ticks;
    rt_u64 di_max_interval_ticks, di_missed_periods, di_due_ticks;
};

void amp_acquisition_init(struct amp_acquisition *acq,
                           const struct amp_acquisition_ops *ops, rt_u32 epoch);
int amp_acquisition_arm(struct amp_acquisition *acq,
                          const struct rt_acq_config_v1 *config, rt_u64 now);
int amp_acquisition_stop(struct amp_acquisition *acq);
void amp_acquisition_host_alive(struct amp_acquisition *acq, rt_u64 now);
int amp_acquisition_poll(struct amp_acquisition *acq, const struct amp_observation *now);

#endif
