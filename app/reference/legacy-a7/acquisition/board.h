#ifndef AMP_ADC_BOARD_H
#define AMP_ADC_BOARD_H
#include "acquisition.h"

#ifndef AMP_SELECT_SETTLE_TICKS
#define AMP_SELECT_SETTLE_TICKS 24U
#endif

struct amp_board {
    const volatile rt_u32 *stop_requested;
    rt_u32 period, mask, touched, running, saved[8];
    /* 仅本核累计，停止后才拷到诊断页，不给正常采样增加共享DDR写事务。 */
    rt_u32 phase_calls, read_phase[2], read_stage, read_chip, start_bound;
    rt_u32 frame_count, frame_max_ticks, frame_last_ticks;
    rt_s32 frame_result;
    rt_u64 frame_sum_ticks;
};
/* 仅故障路径保存快照拒绝原因，正常采样不写共享诊断页。 */
struct amp_observe_failure {
    rt_u32 reason, elapsed, phase, after, period;
};
extern struct amp_observe_failure amp_observe_failure;
#define AMP_OBSERVE_RETRY 1
rt_u64 amp_ticks(void);
int amp_board_prepare(void *context, const struct rt_adc_register_plan *plan);
int amp_board_start(void *context, rt_u32 period);
int amp_board_stop(void *context);
int amp_board_observe(struct amp_board *board, struct amp_observation *out);
int amp_board_read_digital(void *context, rt_u32 *raw, rt_u32 *duration_ticks);
int amp_board_read(void *context, const struct rt_adc_busy_event *event,
                     rt_s16 analog[RT_ANALOG_CHANNELS], rt_u32 *mask, rt_u64 *read_ns);
#endif
