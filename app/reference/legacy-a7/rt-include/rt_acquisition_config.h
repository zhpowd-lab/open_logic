#ifndef RT_ACQUISITION_CONFIG_H
#define RT_ACQUISITION_CONFIG_H

#include "rt_acquisition_rate.h"

#define RT_MPU_ADC_SOURCE_LINUX 0
#define RT_MPU_ADC_SOURCE_E907  1

/* 默认保留 Linux 原有采集；只有实验构建显式选择 E907 后才切换数据来源。 */
#ifndef RT_MPU_ADC_SOURCE
#define RT_MPU_ADC_SOURCE RT_MPU_ADC_SOURCE_LINUX
#endif

/* 原 BSP 在 adc_init 之后才设置 PIT；完整应用首帧须保持原保护的采样几何。 */
#define RT_MPU_INITIAL_SAMPLE_PERIOD_TICKS RT_MPU_CAPTURE_PERIOD_TICKS

#if RT_MPU_ADC_SOURCE != RT_MPU_ADC_SOURCE_LINUX && \
    RT_MPU_ADC_SOURCE != RT_MPU_ADC_SOURCE_E907
#error "RT_MPU_ADC_SOURCE 只能设为 LINUX(0) 或 E907(1)"
#endif

#endif
