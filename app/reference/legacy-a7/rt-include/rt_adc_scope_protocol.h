#ifndef RT_ADC_SCOPE_PROTOCOL_H
#define RT_ADC_SCOPE_PROTOCOL_H

#include "rt_acquisition_diagnostics.h"

#define RT_ADC_SCOPE_OFF 0U
#define RT_ADC_SCOPE_PWM 1U
#define RT_ADC_SCOPE_GPIO 2U
#define RT_ADC_SCOPE_MAX_SECONDS 60U
#define RT_ADC_SCOPE_TICK_HZ 24000000U
#define RT_ADC_SCOPE_PERIOD_TICKS 3750U

/* 开发样机有界输出请求；固定 128 点/50Hz/OS0，仅诊断 CONVST，不授予 ADC 资格。 */
struct rt_adc_scope_request_v1 {
    rt_u32 abi_version, bytes, mode, seconds;
};

/* 观察计数是任务轮询结果；pb8_data 是 DATA 锁存值，不能替代引脚物理电平。 */
struct rt_adc_scope_status_v1 {
    rt_u32 abi_version, bytes, mode, active, seconds, remaining_ms;
    rt_u32 pb8_data, pa4_data, busy_high_observations, busy_low_observations;
    rt_u32 gpio_changes, qualified_frames;
    rt_s32 result;
    struct rt_words64 started_ticks, current_ticks;
    struct rt_adc_pwm_path pwm;
};

/* 普通 R 任务的异步日志；唯一事件环写者发送，IRQ 不格式化或发送日志。 */
struct rt_debug_log_v1 {
    rt_u32 abi_version, bytes, sequence, dropped;
    struct rt_words64 ticks;
    rt_u32 text_bytes, reserved;
    char text[192];
};

RT_ASSERT(sizeof(struct rt_adc_scope_request_v1) == 16, adc_scope_request);
RT_ASSERT(sizeof(struct rt_adc_scope_status_v1) == 108, adc_scope_status);
RT_ASSERT(sizeof(struct rt_debug_log_v1) == 224, debug_log);
#endif
