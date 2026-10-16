/* ADC 短暂读错的本地恢复状态机；调用方负责错误分类与批次提交。 */
#ifndef RT_SAMPLE_RECOVERY_H
#define RT_SAMPLE_RECOVERY_H

#include "rt_shared_abi.h"

#define RT_SAMPLE_RECOVERY_PENDING_MAX 2U
#define RT_SAMPLE_RECOVERY_OUTPUT_MAX  3U
#define RT_SAMPLE_RECOVERY_ERROR_LIMIT 3U

/* 只有 ADC_ERROR 可以等待后续真实采样；结构错误必须立即交给上层处理。 */
#define RT_SAMPLE_INPUT_GOOD        0U
#define RT_SAMPLE_INPUT_ADC_ERROR   1U
#define RT_SAMPLE_INPUT_STRUCTURAL  2U
#define RT_SAMPLE_RECOVERY_OUTPUT      1
#define RT_SAMPLE_RECOVERY_WAIT        0
#define RT_SAMPLE_RECOVERY_FATAL      -1
#define RT_SAMPLE_RECOVERY_INVALID    -2
#define RT_SAMPLE_RECOVERY_STRUCTURAL -3

/* 所有统计按 UINT32_MAX 饱和；连续错误达到三次后锁存，直到显式 reset。 */
/* 恢复诊断计数与连续错误状态；estimated_samples 明确独立于真实 ADC 合格帧。 */
struct rt_sample_recovery_stats {
    rt_u32 adc_errors_total, consecutive_errors, longest_error_run;
    rt_u32 estimated_samples, recovered_bursts, fatal_error_runs;
    rt_u32 startup_unrecoverable;
};

/* 此状态由一个低优先级消费者独占，不放入核间共享内存。 */
struct rt_sample_recovery {
    /* 只存真实 ADC_VALID 锚点，估算值不能再次作为插值基础。 */
    struct rt_sample_v1 previous_good;
    /* 最多保留前两次失败的实际序号/时标；第三连错立即锁存。 */
    struct rt_sample_v1 pending[RT_SAMPLE_RECOVERY_PENDING_MAX];
    struct rt_sample_recovery_stats stats;
    /* faulted 只能通过显式 reset 清除，清累计统计不能绕过容量/故障门槛。 */
    rt_u32 have_previous, pending_count, faulted;
};

/* 输出只包含完整记录，按实际序号排序；skipped_count 明确表示未补出的起始点。 */
struct rt_sample_recovery_batch {
    rt_u32 count, estimated_count, skipped_count;
    struct rt_sample_v1 samples[RT_SAMPLE_RECOVERY_OUTPUT_MAX];
};

/* 清本地恢复状态及计数；换会话、配置或显式解除三连错锁存前调用。 */
void rt_sample_recovery_reset(struct rt_sample_recovery *state);

/*
 * sample 保留采样失败时的实际序号、单调时标及已知时间元数据。
 * GOOD 必须是带 ADC_VALID 的真实点，估算点不得再作为前锚点。
 * 错误后恢复时，一点取前后均值，两点取 1/3 与 2/3 线性插值；
 * 有符号除法向零取整，不使用浮点，不插补数字量，也不伪造 ADC_VALID。
 * 起始无前锚点时跳过待补点，只输出真实恢复点并标记 GAP_BEFORE。
 * 序号必须连续、时标必须递增，session/config/period 和通道数不得跨段变化；
 * 无待补点的真实采样允许 time_epoch 更新，插补禁止跨 RTC 代次。
 * 换配置或重启采样时先 reset。INVALID/STRUCTURAL 不消费输入。
 */
/* 唯一低优先级消费者接收真实或错误点；一两连错等真锚点恢复，三连错锁存 FATAL，估算点不能当真锚点。 */
int rt_sample_recovery_push(struct rt_sample_recovery *state,
                            const struct rt_sample_v1 *sample,
                            rt_u32 input_status,
                            struct rt_sample_recovery_batch *batch);

#endif
