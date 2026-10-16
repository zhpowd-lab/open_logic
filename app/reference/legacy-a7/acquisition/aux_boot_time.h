#ifndef AMP_AUX_BOOT_TIME_H
#define AMP_AUX_BOOT_TIME_H
#include "realtime_io.h"

/* 返回0保留已有Linux UTC，1使用A7 RTC，2继续等外部授时；负值为协议/新鲜度错误。
 * 没有有效RTC时不能保存旧时间冒充本次开机的真实UTC。 */
static inline int amp_aux_boot_utc(const struct amp_rtio_snapshot *s, rt_u32 epoch,
                                 rt_u64 host_seconds, rt_u64 *nanoseconds)
{
    rt_u64 tick=rt_words_value(s->ticks),observed=rt_words_value(s->observed_ticks);
    rt_u64 rtc_tick=rt_words_value(s->rtc_ticks),utc=rt_words_value(s->rtc_utc);
    if(s->magic!=AMP_RTIO_MAGIC || s->version!=AMP_RTIO_VERSION || s->bytes!=sizeof(*s) ||
       s->epoch!=epoch || !s->active || (s->seq_lock&1U)) return -71;
    if(!tick || observed<tick || observed-tick>6000000ULL) return -116;
    if(host_seconds>=1577836800ULL && host_seconds<4102444800ULL) return 0;
    if(s->rtc_error || !(s->flags&AMP_RTIO_RTC_VALID) || !rtc_tick || observed<rtc_tick ||
       observed-rtc_tick>60000000ULL || utc<1577836800ULL || utc>=4102444800ULL) return 2;
    *nanoseconds=utc*1000000000ULL+(observed-rtc_tick)*1000ULL/24ULL;
    return 1;
}
#endif
