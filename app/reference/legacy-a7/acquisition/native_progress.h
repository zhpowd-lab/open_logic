#ifndef AMP_NATIVE_PROGRESS_H
#define AMP_NATIVE_PROGRESS_H
#include "protocol.h"

struct amp_native_progress {
    rt_u64 ticks, sequence, tick_changed_ns, sample_changed_ns;
    rt_u32 epoch, generation;
    int initialized;
};

/* Linux报告的更新时间不能代替裸核推进。容许1ms状态发布和有界调度延迟，
 * 但同会话原生时钟/样点倒退或持续500ms停滞必须显式失败。 */
static inline int amp_native_progress_check(struct amp_native_progress *p,
    const struct amp_acq_status *s, rt_u64 now)
{
    rt_u64 ticks = rt_words_value(s->ticks), sequence = rt_words_value(s->sample_sequence);
    if (!p->initialized) {
        *p = (struct amp_native_progress){ticks, sequence, now, now, s->epoch, s->run_generation, 1};
        return 0;
    }
    if (s->epoch != p->epoch || s->run_generation != p->generation ||
        ticks < p->ticks || sequence < p->sequence || now < p->tick_changed_ns ||
        now < p->sample_changed_ns) return -1;
    if (ticks != p->ticks) p->tick_changed_ns = now;
    if (sequence != p->sequence) p->sample_changed_ns = now;
    p->ticks = ticks; p->sequence = sequence;
    if (now - p->tick_changed_ns >= 500000000U ||
        now - p->sample_changed_ns >= 500000000U) return -1;
    return 0;
}
#endif
