#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "acquisition.h"
#include "rates.h"

static unsigned prepared;
static int prepare(void *context, const struct rt_adc_register_plan *plan)
{
    (void)context;
    assert(plan->enabled_chip_mask == 31U);
    prepared++;
    return 0;
}
static int start(void *context, rt_u32 period)
{
    (void)context;
    assert(amp_rate_points(period));
    return 0;
}
static int stop(void *context) { (void)context; return 0; }
static int read_frame(void *context, const struct rt_adc_busy_event *event,
                      rt_s16 *analog, rt_u32 *mask, rt_u64 *ns)
{
    (void)context; (void)event; (void)analog; (void)mask; (void)ns;
    assert(0); /* 本测试只验证配置边界，不能冒充ADC读取成功。 */
    return RT_LBC_EIO;
}
static int publish(void *context, const struct rt_sample_v1 *sample)
{
    (void)context; (void)sample;
    assert(0);
    return RT_LBC_EIO;
}
static void check(unsigned hz, unsigned ticks, unsigned points, int accepted)
{
    struct amp_acquisition acq;
    struct rt_acq_config_v1 config = {0};
    struct amp_acquisition_ops ops = {
        .prepare=prepare, .start=start, .stop=stop, .read=read_frame, .publish=publish
    };
    unsigned before = prepared;
    amp_acquisition_init(&acq, &ops, 7);
    config.abi_version = RT_ABI_V1;
    config.bytes = sizeof(config);
    config.generation = 1;
    config.initial_period_ticks = ticks;
    config.enabled_chip_mask = 31;
    config.samples_per_cycle = points;
    memset(config.range_code, 3, sizeof(config.range_code));
    config.crc32 = rt_crc32(&config, sizeof(config));
    if (accepted) {
        assert(amp_rate_period(hz) == ticks && amp_rate_points(ticks) == points);
        assert(points * 50U == hz);
        if (hz == 25600U) assert(ticks == 938U && (rt_u64)hz * ticks == 24012800U);
        else assert((rt_u64)hz * ticks == 24000000U);
        assert(!amp_acquisition_arm(&acq, &config, 0));
        assert(prepared == before + 1 && acq.qualified == 0);
        assert(acq.plan.period_ticks == ticks);
        assert(!amp_acquisition_stop(&acq));
        /* 同样CRC合法但几何不符的配置，必须在硬件prepare前拒绝。 */
        config.samples_per_cycle++;
        config.crc32 = 0;
        config.crc32 = rt_crc32(&config, sizeof(config));
        assert(amp_acquisition_arm(&acq, &config, 0) == RT_REPLY_BAD_CONFIG);
        assert(prepared == before + 1);
    } else {
        assert((!amp_rate_period(hz) || amp_rate_period(hz) != ticks) && !amp_rate_points(ticks));
        assert(amp_acquisition_arm(&acq, &config, 0) == RT_REPLY_BAD_CONFIG);
        assert(prepared == before);
    }
}
int main(void)
{
    check(6400, 3750, 128, 1);
    check(12800, 1875, 256, 1);
    check(16000, 1500, 320, AMP_HIGH_RATE_EXPERIMENT);
    check(19200, 1250, 384, AMP_HIGH_RATE_EXPERIMENT);
    check(20000, 1200, 400, AMP_HIGH_RATE_EXPERIMENT);
    check(25600, 937, 512, 0);
    check(25600, 938, 512, AMP_512_EXPERIMENT);
    check(0, 0, 0, 0);
    assert(!amp_rate_period(20001) && !amp_rate_points(1199));
    printf("{\"high_rate_experiment\":%d,\"points512_experiment\":%d,\"configuration_cases\":8,\"adc_hardware_tested\":false}\n",
           AMP_HIGH_RATE_EXPERIMENT, AMP_512_EXPERIMENT);
    return 0;
}
