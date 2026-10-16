#ifndef RT_SOFTWARE_CLOCK_H
#define RT_SOFTWARE_CLOCK_H

#include "rt_shared_abi.h"

/* 同核软件 UTC 双银行，任务写非活动 bank 再换 active，中断读取；stamped_epoch 标记首个新代次样点。 */
struct rt_soft_rtc {
    /* 任务写非活动银行；同核中断读取活动银行，不等待任务持有的锁。 */
    struct rt_soft_rtc_anchor bank[2];
    /* 屏障后切换的本核银行号，仅为 0 或 1，不是共享区指针。 */
    volatile rt_u32 active;
    /* 上次已标时的 UTC 代次，用于首点 RT_SAMPLE_TIME_CHANGED 标记。 */
    rt_u32 stamped_epoch;
};

/* 任务是唯一写者，中断是唯一读者；同核中断不会等待任务持有的锁。 */
/* 任务唯一写者校验锚点与代次后写非活动银行，再发布 active；不是 PPS 锁定，不等待 ISR。 */
static inline int rt_soft_rtc_set(struct rt_soft_rtc *rtc,
                                  const struct rt_soft_rtc_anchor *anchor,
                                  rt_u64 now)
{
    rt_u64 tick = rt_words_value(anchor->captured_ticks);
    rt_u64 seconds = rt_words_value(anchor->utc_seconds);
    rt_u32 next = rtc->active ^ 1U;

    if (!anchor->time_epoch || anchor->source != RT_RTC_SOURCE_LINUX ||
        anchor->utc_nanoseconds >= 1000000000U ||
        seconds < 946684800ULL || seconds >= 4102444800ULL ||
        tick > now || now - tick > 24000000ULL * 60U ||
        (rtc->bank[rtc->active].time_epoch &&
         anchor->time_epoch == rtc->bank[rtc->active].time_epoch))
        return RT_REPLY_BAD_CONFIG;
    rtc->bank[next] = *anchor;
    __sync_synchronize();
    rtc->active = next;
    return RT_REPLY_OK;
}

/* 中断唯一读者以样点单调时标换算 UTC 并标时间代次变化；无锚点保留无效，不伪造 PPS_LOCKED。 */
static inline void rt_soft_rtc_stamp(struct rt_soft_rtc *rtc,
                                      struct rt_sample_v1 *sample)
{
    struct rt_soft_rtc_anchor anchor = rtc->bank[rtc->active];
    rt_u64 ticks = rt_words_value(sample->sample_ticks);
    rt_u64 base = rt_words_value(anchor.captured_ticks), delta, seconds;
    rt_u32 nanoseconds, fraction;

    sample->utc_seconds = rt_words_make(0);
    sample->utc_nanoseconds = sample->time_epoch = 0;
    sample->flags &= ~(RT_SAMPLE_UTC_VALID | RT_SAMPLE_PPS_LOCKED |
                       RT_SAMPLE_HOLDOVER | RT_SAMPLE_TIME_CHANGED);
    if (!anchor.time_epoch)
        return;
    seconds = rt_words_value(anchor.utc_seconds);
    /* 24 MHz 的余数先缩放，避免累计计数乘 10^9 溢出；全过程不用浮点。 */
    if (ticks >= base) {
        delta = ticks - base;
        seconds += delta / 24000000U;
        fraction = (rt_u32)(delta % 24000000U) * 125U / 3U;
        nanoseconds = anchor.utc_nanoseconds + fraction;
        if (nanoseconds >= 1000000000U) {
            seconds++;
            nanoseconds -= 1000000000U;
        }
    } else {
        delta = base - ticks;
        if (delta / 24000000U >= seconds)
            return;
        seconds -= delta / 24000000U;
        fraction = (rt_u32)(delta % 24000000U) * 125U / 3U;
        nanoseconds = anchor.utc_nanoseconds;
        if (fraction > nanoseconds) {
            seconds--;
            nanoseconds += 1000000000U;
        }
        nanoseconds -= fraction;
    }
    sample->utc_seconds = rt_words_make(seconds);
    sample->utc_nanoseconds = nanoseconds;
    sample->time_epoch = anchor.time_epoch;
    sample->flags |= RT_SAMPLE_UTC_VALID;
    if (rtc->stamped_epoch != anchor.time_epoch) {
        sample->flags |= RT_SAMPLE_TIME_CHANGED;
        rtc->stamped_epoch = anchor.time_epoch;
    }
}

#endif
