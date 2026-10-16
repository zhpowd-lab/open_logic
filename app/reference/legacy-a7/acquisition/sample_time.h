#ifndef AMP_SAMPLE_TIME_H
#define AMP_SAMPLE_TIME_H
#include "rt_software_clock.h"

/* 2^32=3*1431655765+1，拆成32bit常数除法，结果保持原floor(ticks*125/3)。
 * 裸机ARM32避免通用64bit软件除法的输入相关耗时。 */
static inline rt_u64 amp_ticks_to_ns(rt_u64 ticks)
{
    rt_u32 hi = (rt_u32)(ticks >> 32), lo = (rt_u32)ticks;
    rt_u32 remainder = hi % 3U + lo % 3U;
    rt_u64 quotient = ((rt_u64)(hi / 3U) << 32) + lo / 3U +
        (hi % 3U) * 1431655765U + remainder / 3U;
    return quotient * 125U + remainder % 3U * 125U / 3U;
}

/* A7私有UTC累加器：保留24MHz的1/3ns余数，逐帧结果与原锚点整除相同。
 * 连续帧用32bit小增量计算，避免每帧按不断增长的64bit相对时标做软件除法。 */
struct amp_sample_time {
    rt_u64 ticks, seconds;
    rt_u32 nanoseconds, fraction, epoch;
};
static inline void amp_sample_time_reset(struct amp_sample_time *time,
                                          const struct rt_soft_rtc_anchor *anchor)
{
    time->ticks = rt_words_value(anchor->captured_ticks);
    time->seconds = rt_words_value(anchor->utc_seconds);
    time->nanoseconds = anchor->utc_nanoseconds;
    time->fraction = 0;
    time->epoch = anchor->time_epoch;
}
static inline void amp_sample_time_stamp(struct amp_sample_time *time,
                                          struct rt_soft_rtc *rtc, struct rt_sample_v1 *sample)
{
    rt_u64 ticks = rt_words_value(sample->sample_ticks);
    rt_u32 numerator, nanoseconds;
    const struct rt_soft_rtc_anchor *anchor = &rtc->bank[rtc->active];
    if (!time->epoch || time->epoch != anchor->time_epoch || ticks < time->ticks ||
        ticks - time->ticks >= 24000000U) {
        /* 断续/倒退/换锚点按原函数精确处理，不用累加器掩盖时钟异常。 */
        rt_soft_rtc_stamp(rtc, sample);
        rt_u64 base = rt_words_value(anchor->captured_ticks);
        time->epoch = 0;
        if ((sample->flags & RT_SAMPLE_UTC_VALID) && ticks >= base) {
            time->ticks = ticks;
            time->seconds = rt_words_value(sample->utc_seconds);
            time->nanoseconds = sample->utc_nanoseconds;
            time->fraction = (rt_u32)((ticks - base) % 3U) * 2U % 3U;
            time->epoch = anchor->time_epoch;
        }
        return;
    }
    numerator = (rt_u32)(ticks - time->ticks) * 125U + time->fraction;
    nanoseconds = time->nanoseconds + numerator / 3U;
    time->fraction = numerator % 3U;
    time->ticks = ticks;
    if (nanoseconds >= 1000000000U) {
        nanoseconds -= 1000000000U;
        time->seconds++;
    }
    time->nanoseconds = nanoseconds;
    sample->utc_seconds = rt_words_make(time->seconds);
    sample->utc_nanoseconds = nanoseconds;
    sample->time_epoch = time->epoch;
    sample->flags &= ~(RT_SAMPLE_PPS_LOCKED | RT_SAMPLE_HOLDOVER | RT_SAMPLE_TIME_CHANGED);
    sample->flags |= RT_SAMPLE_UTC_VALID;
    if (rtc->stamped_epoch != time->epoch) {
        sample->flags |= RT_SAMPLE_TIME_CHANGED;
        rtc->stamped_epoch = time->epoch;
    }
}
#endif
