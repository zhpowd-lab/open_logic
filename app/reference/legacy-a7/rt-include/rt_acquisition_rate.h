#ifndef RT_ACQUISITION_RATE_H
#define RT_ACQUISITION_RATE_H

#include "rt_shared_abi.h"
#ifdef __KERNEL__
#include <linux/math64.h>
#endif

/* 两端共用速率定义；硬件计数器只能表示整数 tick，实际频率以周期回读为准。 */
#ifndef RT_SAMPLE_CLOCK_HZ
#define RT_SAMPLE_CLOCK_HZ 24000000U
#endif
#ifndef RT_MPU_NOMINAL_LINE_HZ
#define RT_MPU_NOMINAL_LINE_HZ 50U
#endif
#ifndef RT_MPU_CAPTURE_SAMPLES_PER_CYCLE
#define RT_MPU_CAPTURE_SAMPLES_PER_CYCLE 128U
#endif
/* 点数步长属于采集契约，两核必须一致；当前装置按 128 点整数倍配置。 */
#ifndef RT_MPU_CAPTURE_POINTS_STEP
#define RT_MPU_CAPTURE_POINTS_STEP 128U
#endif
#ifndef RT_MPU_ACQUISITION_BATCH_READ_PERIOD_US
#define RT_MPU_ACQUISITION_BATCH_READ_PERIOD_US 2000U
#endif

#define RT_MPU_CAPTURE_HZ \
    (RT_MPU_NOMINAL_LINE_HZ * RT_MPU_CAPTURE_SAMPLES_PER_CYCLE)
#define RT_MPU_CAPTURE_PERIOD_TICKS \
    ((RT_SAMPLE_CLOCK_HZ + RT_MPU_CAPTURE_HZ / 2U) / RT_MPU_CAPTURE_HZ)
#define RT_MPU_CAPTURE_BATCH_FRAMES \
    ((1ULL * RT_MPU_CAPTURE_HZ * RT_MPU_ACQUISITION_BATCH_READ_PERIOD_US + \
      999999ULL) / 1000000ULL)

#if RT_SAMPLE_CLOCK_HZ != 24000000U
#error "当前 ABI 的 sample_ticks 固定为 24 MHz；其它硬件时钟须在适配层换算"
#endif
#if RT_MPU_CAPTURE_POINTS_STEP < 1U || RT_MPU_CAPTURE_POINTS_STEP > 65536U
#error "采样点数步长超出有效范围"
#endif
#if RT_MPU_NOMINAL_LINE_HZ < 1U || RT_MPU_NOMINAL_LINE_HZ > 1000U || \
    RT_MPU_CAPTURE_SAMPLES_PER_CYCLE < RT_MPU_CAPTURE_POINTS_STEP || \
    RT_MPU_CAPTURE_SAMPLES_PER_CYCLE % RT_MPU_CAPTURE_POINTS_STEP != 0U || \
    RT_MPU_CAPTURE_SAMPLES_PER_CYCLE > 65536U
#error "采样速率参数超出整数换算的有效范围"
#endif
#if RT_MPU_ACQUISITION_BATCH_READ_PERIOD_US < 1U || \
    RT_MPU_ACQUISITION_BATCH_READ_PERIOD_US > 1000000U
#error "批读周期超出有效范围"
#endif

/* 实验入口按点数计算最接近的固定周期，返回零表示参数无法表达。 */
/* 将线频与每周波 128 整数倍点数换成最接近的 24 MHz 周期；不可表达参数返回零。 */
static inline rt_u32 rt_capture_period_ticks(rt_u32 line_hz,
                                             rt_u32 points_per_cycle)
{
    rt_u64 rate = (rt_u64)line_hz * points_per_cycle;

    if (!line_hz || points_per_cycle < RT_MPU_CAPTURE_POINTS_STEP ||
        points_per_cycle > 65536U || points_per_cycle % RT_MPU_CAPTURE_POINTS_STEP ||
        rate > RT_SAMPLE_CLOCK_HZ)
        return 0U;
#ifdef __KERNEL__
    /* ARM32 内核没有链接 C 运行库的 64 位除法，使用内核提供的整数接口。 */
    return (rt_u32)div_u64(RT_SAMPLE_CLOCK_HZ + rate / 2U, (rt_u32)rate);
#else
    return (rt_u32)((RT_SAMPLE_CLOCK_HZ + rate / 2U) / rate);
#endif
}

#endif
