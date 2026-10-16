#ifndef RT_ACQUISITION_DIAGNOSTICS_H
#define RT_ACQUISITION_DIAGNOSTICS_H

#include "rt_shared_abi.h"

/* READ_STATUS 的可选载荷，不改变共享帧、目录或已有状态的布局。 */
struct rt_acquisition_timing_v1 {
    rt_u32 abi_version, bytes, period_ticks, tick_hz;
    rt_u32 ready_ticks, read_budget_ticks, minimum_period_ticks, digital_divider;
    /* isr_* 在 PIO 下为整次回调，在 DMA 下为 Timer6 单帧暂存；批发布另见 publish_*。 */
    struct rt_words64 frames, read_sum_ticks, isr_sum_ticks;
    /* 读取耗时与 ISR 墙钟耗时分开记录，均含各自测量窗口内的抢占影响。 */
    rt_u32 read_min_ticks, read_max_ticks, isr_min_ticks, isr_max_ticks;
    rt_u32 deadline_overruns, maximum_period_ticks;
    rt_u32 enabled_chip_mask, oversampling_code;
};

/* READ_STATUS 可先带选择参数查询预算，随后才提交采集配置。 */
struct rt_acquisition_selection_v1 {
    rt_u32 enabled_chip_mask, oversampling_code;
    rt_u32 capture_trigger;
};

/* 独立查询触发与恢复状态，避免扩展已有 224 字节 DMA 载荷。 */
struct rt_acquisition_capture_v1 {
    rt_u32 abi_version, bytes, capture_trigger, tick_hz;
    /* 真 BUSY 回调、依据 PWM 周期判出的漏周期与重复沿；自检不计入这些字段。 */
    rt_u32 busy_edges, busy_missing_cycles, busy_duplicate_edges;
    rt_u32 busy_interval_max_ticks;
    rt_u32 adc_error_total, adc_error_streak, adc_error_streak_max;
    /* 估算数量/恢复事件/待恢复失败帧，不能与原始 ADC_VALID 合格数量混算。 */
    rt_u32 adc_estimated_frames, adc_recovered_events, adc_pending_errors;
    rt_u32 adc_fatal_errors, adc_startup_unrecoverable;
    rt_u32 busy_owner_source, busy_pending_events;
    /* startup_stage 定位最后准备阶段；高低观察只说明软件取样结果，不代替物理波形。 */
    rt_u32 startup_stage, busy_high_observations, busy_low_observations;
    rt_u32 busy_measured_cycles, busy_measured_min, busy_measured_max;
    rt_u32 busy_measured_rejected;
    rt_u32 busy_plain_high, busy_plain_low, busy_plain_falls;
    rt_u32 busy_eint_high, busy_eint_low, busy_eint_pending;
    rt_u32 busy_gpio_trials, busy_gpio_high_trials, busy_gpio_fall_trials;
    rt_s32 busy_gpio_result;
    /* 有限 GPIO 诊断的请求高电平宽度，按 tick_hz 换算，不代表示波器测量值。 */
    rt_u32 busy_gpio_high_ticks;
    /* BUSY PIO 的缺沿监测；每次真实 BUSY 到达都重装 150% 采样间隔。 */
    rt_u32 watchdog_period_ticks, watchdog_resets, watchdog_checks;
    rt_u32 watchdog_timeouts, watchdog_stale;
};
#define RT_ACQUISITION_CAPTURE_LEGACY_BYTES 144U
RT_ASSERT(sizeof(struct rt_acquisition_capture_v1) == 164, acquisition_capture);
RT_ASSERT(offsetof(struct rt_acquisition_capture_v1, watchdog_period_ticks) == 144,
          acquisition_capture_watchdog);
RT_ASSERT(sizeof(struct rt_acquisition_capture_v1) <= 224, acquisition_capture_payload);

/* 独立中断分发诊断，不计入真实 BUSY、采样序号或 ADC 合格帧。 */
struct rt_adc_busy_dispatch_test {
    rt_s32 rc;
    rt_u32 irqs, gpio_pending, gpio_level, clic_state;
    rt_u32 mstatus, mie, mip, mode, polls, elapsed_ticks, source_pending;
};
RT_ASSERT(sizeof(struct rt_adc_busy_dispatch_test) == 48, busy_dispatch_test);

/* 固定 PWM 运行时的只读信号快照；相位两次读回不能代替引脚波形测量。 */
struct rt_adc_pwm_path {
    rt_u32 pb_cfg, pwm_mux, pwm_clock, pwm_gate, pwm_enable;
    rt_u32 pwm_control, pwm_period, pwm_active, phase_before, phase_after;
};
RT_ASSERT(sizeof(struct rt_adc_pwm_path) == 40, adc_pwm_path);

/* READ_BUSY_PATH 的可选诊断载荷；硬件快照仅用于本平台定位。 */
struct rt_busy_irq_path_v1 {
    rt_u32 abi_version, bytes, executed_mask;
    struct rt_adc_busy_dispatch_test tests[3];
    rt_u32 gpio_cfg, gpio_mode, gpio_ctrl, gpio_pending;
    rt_u32 gpio_debounce, gpio_data, mintthresh;
    struct rt_adc_pwm_path pwm;
};
RT_ASSERT(sizeof(struct rt_busy_irq_path_v1) == 224, busy_irq_path);
RT_ASSERT(sizeof(struct rt_busy_irq_path_v1) <= 224, busy_irq_path_payload);

/* 本版诊断载荷最多五片、每片十三个配置寄存器；不改变已有样点布局。 */
#define RT_ADC_REGISTER_DIAG_MAX_CHIPS 5U
#define RT_ADC_REGISTER_DIAG_REGISTERS 13U
/* 独立配置寄存器诊断载荷，记录使能/尝试片、逐项回读、首错及恢复失败；不携带原始样点。 */
struct rt_adc_register_snapshot_v1 {
    rt_u32 abi_version, bytes, generation, enabled_chip_mask;
    rt_u32 captured_chip_mask, comparisons, mismatches, restore_failures;
    rt_s32 result;
    /* 无错误时 first_chip/first_register 为 0xffffffff，恢复数据模式失败用寄存器 0xff。 */
    rt_u32 first_chip, first_register, first_actual, first_expected;
    /* 顺序为 mode(2)、range(4..7)、sequence(0x20..0x27)，保留未掩码的总线读数。 */
    rt_u16 values[RT_ADC_REGISTER_DIAG_MAX_CHIPS][RT_ADC_REGISTER_DIAG_REGISTERS];
    rt_u16 reserved;
};
RT_ASSERT(sizeof(struct rt_adc_register_snapshot_v1) == 184, adc_register_snapshot);
RT_ASSERT(offsetof(struct rt_adc_register_snapshot_v1, values) == 52,
          adc_register_snapshot_values);
RT_ASSERT(sizeof(struct rt_adc_register_snapshot_v1) <= 224, adc_register_snapshot_payload);

/* Timer3 周期 GPIO 触发的有限对照；计数只用于定位，不能增加原始帧或启动资格。 */
#define RT_ADC_TIMER_TRIGGER_TRIALS 64U
/* 有限Timer3/GPIO转换对照结果，保存脉冲读回、真实BUSY观察和资源恢复证据；不能授予采集资格。 */
struct rt_adc_timer_trigger_v1 {
    rt_u32 abi_version, bytes, period_ticks, high_ticks;
    rt_u32 trials, timer_irqs, gpio_requests, gpio_high_matches, gpio_low_matches;
    rt_u32 busy_high, busy_falls, busy_pending, busy_callbacks;
    rt_u32 timer_interval_min_ticks, timer_interval_max_ticks;
    rt_u32 irq_enable_before, irq_enable_after, elapsed_ticks;
    rt_s32 result;
    rt_u32 restore_failures, pb_cfg_after, pa_data_after;
};
RT_ASSERT(sizeof(struct rt_adc_timer_trigger_v1) == 88, adc_timer_trigger);
RT_ASSERT(offsetof(struct rt_adc_timer_trigger_v1, result) == 72, adc_timer_trigger_result);
RT_ASSERT(sizeof(struct rt_adc_timer_trigger_v1) <= 224, adc_timer_trigger_payload);

/* DMA 完成回调的墙钟耗时，包含更高优先级中断的抢占时间。 */
struct rt_dma_irq_timing {
    struct rt_words64 sum_ticks;
    rt_u32 calls, min_ticks, max_ticks;
    /* 保留最后一次窗口拒绝的现场；reason=1 为启动，reason=2 为完成。 */
    rt_u32 fault_phase, fault_period, fault_elapsed, fault_reason;
};

/* 独立查询扩展统计；旧 READ_STATUS 和样点布局保持不变。 */
struct rt_acquisition_dma_v1 {
    rt_u32 abi_version, bytes, enabled, timer_calls;
    struct rt_words64 timer_sum_ticks;
    rt_u32 timer_max_ticks, dma_max_ticks, pipeline_max_ticks, pending0, pending1;
    /* 到期至软件入口读计数器的分桶；BUSY 无硬件边沿捕获时不得冒充物理入口延迟。 */
    rt_u32 irq_histogram[16];
    rt_u32 dma_config, dma_remaining, dma_irq_pending, dma_destination;
    struct rt_words64 irq_sum_ticks;
    rt_u32 irq_min_ticks, timer_min_ticks;
    struct rt_dma_irq_timing completion;
    /* 低优先级发布单独统计；默认 2ms，不包含 Linux 另一层 2ms 批读的额外数据年龄。 */
    rt_u32 deferred_publish, publish_period_ticks, publish_calls;
    struct rt_words64 publish_sum_ticks;
    /* 本地 128 槽容量与最大积压；溢出必须失败，不能覆盖消费者仍持有的槽。 */
    rt_u32 publish_max_ticks, queue_max_depth, queue_overflows, digital_enabled;
    rt_u32 busy_min_ticks, busy_max_ticks;
    rt_u32 integer_isr;
};

RT_ASSERT(sizeof(struct rt_acquisition_dma_v1) == 224, acquisition_dma);

RT_ASSERT(sizeof(struct rt_acquisition_timing_v1) == 88, acquisition_timing);
RT_ASSERT(sizeof(struct rt_acquisition_selection_v1) == 12, acquisition_selection);
RT_ASSERT(offsetof(struct rt_acquisition_timing_v1, frames) == 32, timing_frames);
RT_ASSERT(offsetof(struct rt_acquisition_timing_v1, isr_max_ticks) == 68, timing_isr_max);

#endif
