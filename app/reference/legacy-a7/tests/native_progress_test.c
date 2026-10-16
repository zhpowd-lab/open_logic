#include <assert.h>
#include <stdio.h>
#include "native_progress.h"
int main(void)
{
    struct amp_native_progress p = {0};
    struct amp_acq_status s = {.epoch=7, .run_generation=1};
    s.ticks = rt_words_make(200); s.sample_sequence = rt_words_make(10);
    assert(!amp_native_progress_check(&p, &s, 1));
    assert(!amp_native_progress_check(&p, &s, 499999999));
    /* 仅Linux时钟变化，不得将缓存状态认作裸核存活。 */
    assert(amp_native_progress_check(&p, &s, 500000001));
    p.initialized=0;
    assert(!amp_native_progress_check(&p, &s, 1));
    s.ticks=rt_words_make(300); s.sample_sequence=rt_words_make(11);
    assert(!amp_native_progress_check(&p, &s, 500000000));
    s.ticks=rt_words_make(299);
    assert(amp_native_progress_check(&p, &s, 500000001));
    s.ticks=rt_words_make(301); s.epoch++;
    assert(amp_native_progress_check(&p, &s, 500000002));
    puts("Native progress: cached/advancing/backward/session cases passed");
    return 0;
}
