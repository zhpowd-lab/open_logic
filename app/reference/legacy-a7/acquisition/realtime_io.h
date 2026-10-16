/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMP_REALTIME_IO_H
#define AMP_REALTIME_IO_H
#include "rt_shared_abi.h"
#ifndef AMP_REALTIME_IO
#define AMP_REALTIME_IO 0
#endif
#define AMP_RTIO_OFFSET 0x600U
#define AMP_RTIO_COMMAND_OFFSET 0xe00U
#define AMP_RTIO_MAGIC 0x314f4952U
#define AMP_RTIO_VERSION 1U
#define AMP_RTIO_IRQ 51U
#define AMP_RTIO_RTC_SET 1U
#define AMP_RTIO_RTC_VALID 1U
#define AMP_RTIO_GPS_PERIOD_VALID 2U
#define AMP_RTIO_FREQ0_CONFLICT 4U
#define AMP_RTIO_CAPTURE_FAULT 8U
/* capture[0/1]为GPS/RTC秒沿，[2/3]为原业务测频0/1；全部周期为1.5MHz计数。 */
struct amp_rtio_capture {
    struct rt_words64 count, last_ticks;
    rt_u32 period;
    rt_s32 error;
};
/* 固定宽度、版本和独立seqlock；不跨核传递Linux对象或指针。 */
struct amp_rtio_snapshot {
    rt_u32 magic, version, bytes, seq_lock;
    rt_u32 epoch, run_generation, active, flags;
    struct rt_words64 ticks, rtc_utc, rtc_ticks;
    rt_s32 rtc_error, temperature_error;
    rt_u32 rtc_reads, rtc_bus_errors, rtc_ctrl, rtc_request_id;
    rt_s32 rtc_request_result, temperature_milli_c;
    struct amp_rtio_capture capture[4];
    /* 槽0..3=GPADC2 ch0..3，槽4..6=GPADC0 ch4..6；不冒充旧逻辑7/8。 */
    rt_u32 gpadc_raw[7], gpadc_valid, gpadc_updates[2];
    struct rt_words64 gpadc_ticks[7];
    rt_u32 service_max_ticks, irq_max_ticks, unexpected_capture;
    struct rt_words64 temperature_ticks;
    rt_u32 publish_max_ticks;
    /* 仅Linux ioctl填充副本，用于同一24MHz计数器的新鲜度和CLOCK_MONOTONIC换算。 */
    struct rt_words64 observed_ticks, observed_monotonic_ns;
};
struct amp_rtio_command {
    rt_u32 version, bytes, request_id, epoch, operation;
    rt_u32 year, month, day, hour, minute, second, weekday;
    rt_u32 crc32;
};
RT_ASSERT(sizeof(struct amp_rtio_snapshot) <= 512, amp_rtio_snapshot_bounds);
RT_ASSERT(sizeof(struct amp_rtio_command) == 52, amp_rtio_command_size);
/* 不访问硬件的日历校验也用于Linux入口和宿主回归。 */
int amp_rtio_decode_rtc(const rt_u8 raw[16], rt_u64 *utc);
int amp_rtio_encode_rtc(const struct amp_rtio_command *command, rt_u8 raw[7]);
#if AMP_REALTIME_IO
void amp_rtio_init(rt_u32 epoch);
int amp_rtio_start(void);
int amp_rtio_stop(void);
void amp_rtio_service(rt_u64 now);
void amp_rtio_irq(rt_u64 now);
void amp_rtio_publish(rt_u32 run);
#endif
#endif
