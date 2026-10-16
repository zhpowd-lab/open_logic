#ifndef RT_ACQUISITION_H
#define RT_ACQUISITION_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

/* CPU2 每次入口只消费本轮开始时已发布的帧。 */
/* Linux CPU2 消费本轮入口已发布批次并回放旧采集回调；回调不可睡眠，timer_delay 保留现有框架参数。 */
int rt_acquisition_consume_batch(void (*consume)(unsigned long),
                              unsigned long timer_delay);
/* 查询原始连续帧及采集状态是否具备保护运行门槛；时间新鲜度约束仍由实际实现维护。 */
bool rt_acquisition_protection_ready(void);
/* CPU3 与整批样点回放互斥；调用方不得在回调中睡眠或操作网络。 */
/* Linux CPU3 在整批回放互斥条件下执行原 5ms 保护回调；不得在回调中睡眠或操作网络。 */
void rt_acquisition_protect(void (*consume)(unsigned long), unsigned long delay);
/* 申请下一批定时周期；定值在远端确认后才作为有效配置使用。 */
/* 申请远端周期定值，确认后才成为有效配置；只传递整数 tick，不由 Linux 重写 ADC 总线。 */
int rt_acquisition_period_request(unsigned int period_ticks);

#endif
