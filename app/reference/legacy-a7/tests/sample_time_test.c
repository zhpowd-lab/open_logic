#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sample_time.h"

int main(void)
{
    struct rt_soft_rtc fast = {0}, reference = {0};
    struct amp_sample_time cache;
    struct rt_sample_v1 actual = {0}, expected = {0};
    const rt_u64 base = (1ULL << 40) + 123456;
    rt_u64 tick; unsigned i, scenario, cases = 0;
    rt_u64 random = 1;
    for (i = 0; i < 1000000; i++) {
        random = random * 6364136223846793005ULL + 1U;
        /* 原时标纳秒不溢出的范围，并覆盖多个32bit进位。 */
        rt_u64 value = random % 400000000000000000ULL;
        assert(amp_ticks_to_ns(value) == value / 3U * 125U + value % 3U * 125U / 3U);
    }
    for (scenario = 0; scenario < 5; scenario++) {
        memset(&fast, 0, sizeof(fast));
        fast.bank[0].captured_ticks = rt_words_make(base);
        fast.bank[0].utc_seconds = rt_words_make(1700000000);
        fast.bank[0].utc_nanoseconds = 999999999;
        fast.bank[0].time_epoch = scenario == 4 ? 0 : 7;
        reference = fast;
        amp_sample_time_reset(&cache, &fast.bank[0]);
        tick = base + (scenario == 1 ? 24000001U : 0U);
        for (i = 0; i < 70000; i++) {
            if (scenario == 2 && i == 20000) {
                fast.bank[0].time_epoch++; reference.bank[0].time_epoch++;
                fast.bank[0].utc_nanoseconds = reference.bank[0].utc_nanoseconds = 123456789;
            }
            if (scenario == 3 && i < 4) tick = base - 4U + i;
            else tick += i & 1U ? 937U : 938U;
            actual.flags = expected.flags = RT_SAMPLE_ADC_VALID | RT_SAMPLE_EDGE_PREDICTED;
            actual.sample_ticks = expected.sample_ticks = rt_words_make(tick);
            amp_sample_time_stamp(&cache, &fast, &actual);
            rt_soft_rtc_stamp(&reference, &expected);
            assert(rt_words_value(actual.utc_seconds) == rt_words_value(expected.utc_seconds));
            assert(actual.utc_nanoseconds == expected.utc_nanoseconds);
            assert(actual.time_epoch == expected.time_epoch && actual.flags == expected.flags);
            cases++;
        }
    }
    printf("A7 UTC accumulator/reference comparisons passed: %u\n", cases);
    return 0;
}
