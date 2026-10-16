#ifndef AMP_SERVICE_WINDOW_H
#define AMP_SERVICE_WINDOW_H
#include "rt_shared_abi.h"

/* ticks先于相位读出，按经过时间求保守剩余量。后台事务不得跨本次周期，
 * 也不能挤掉下次BUSY轮询；无余量时延后诊断/租约，绝不跳过采样观测。 */
static inline int amp_service_window(rt_u32 period, rt_u32 phase,
                                      rt_u64 observed, rt_u64 now, rt_u32 reserve)
{
    if (!period || phase >= period || now < observed) return 0;
    rt_u64 elapsed = now - observed;
    rt_u32 remaining = period - phase;
    return elapsed < remaining && remaining - elapsed >= reserve;
}
#endif
