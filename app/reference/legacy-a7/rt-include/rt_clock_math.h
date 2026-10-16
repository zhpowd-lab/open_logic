#ifndef RT_CLOCK_MATH_H
#define RT_CLOCK_MATH_H

#include "rt_shared_abi.h"

/* 先除后乘，避免 SDK 累计计数乘固定点系数在运行数十分钟后溢出。 */
/* 先除后乘作整数时钟域换算，零源频率返回零；同频直接保留计数避免 ISR 宽除法。 */
static inline rt_u64 rt_clock_scale(rt_u64 ticks, rt_u32 from, rt_u32 to)
{
    if (!from)
        return 0U;
    /* 同频时直接保留计数，避免采样 ISR 反复执行不必要的 64 位除法。 */
    if (from == to)
        return ticks;
    return (ticks / from) * to + (ticks % from) * to / from;
}

#endif
