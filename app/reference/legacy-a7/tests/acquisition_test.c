#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "acquisition.h"

struct model {
    struct amp_acquisition acq;
    rt_u32 reads, publishes, stops, mask;
    int read_error, publish_error, stop_error, prepare_error;
    int di_error;
    rt_u32 di_reads;
    struct rt_sample_v1 last;
};
static int prepare(void *context, const struct rt_adc_register_plan *plan)
{
    struct model *m = context;
    m->mask = plan->enabled_chip_mask;
    return m->prepare_error;
}
static int start(void *context, rt_u32 period)
{
    (void)context;
    assert(period == 3750U || period == 1875U);
    return 0;
}
static int stop(void *context)
{
    struct model *m = context;
    m->stops++;
    return m->stop_error;
}
static int read_frame(void *context, const struct rt_adc_busy_event *event,
                        rt_s16 analog[RT_ANALOG_CHANNELS], rt_u32 *mask, rt_u64 *read_ns)
{
    struct model *m = context;
    rt_u32 chip, channel;
    assert(event->phase == 200U);
    m->reads++;
    assert(m->di_reads == m->reads);
    for (chip = 0; chip < RT_ADC_CHIPS; chip++)
        for (channel = 0; channel < 16U; channel++)
            analog[chip * 16U + channel] = m->mask & (1U << chip) ?
                (rt_s16)(-12000 + (int)chip * 1000 + (int)channel) : 0;
    *mask = m->mask;
    *read_ns = 30000U;
    return m->read_error;
}
static int read_digital(void *context, rt_u32 *raw, rt_u32 *duration)
{
    struct model *m = context;
    assert(m->reads == m->di_reads);
    m->di_reads++;
    *raw = 0xa55aU; *duration = 60;
    return m->di_error;
}
static int publish(void *context, const struct rt_sample_v1 *sample)
{
    struct model *m = context;
    m->publishes++;
    m->last = *sample;
    return m->publish_error;
}
static struct rt_acq_config_v1 config(rt_u32 mask)
{
    struct rt_acq_config_v1 c = {0};
    c.abi_version = RT_ABI_V1;
    c.bytes = sizeof(c);
    c.generation = 1;
    c.initial_period_ticks = 3750;
    c.enabled_chip_mask = mask;
    c.samples_per_cycle = 128;
    memset(c.range_code, 3, sizeof(c.range_code));
    c.crc32 = rt_crc32(&c, sizeof(c));
    return c;
}
static void init(struct model *m, rt_u32 mask)
{
    struct rt_acq_config_v1 c = config(mask);
    struct amp_acquisition_ops ops = {
        .context = m, .prepare = prepare, .start = start, .stop = stop,
        .read = read_frame, .publish = publish,
        .read_digital = read_digital,
    };
    memset(m, 0, sizeof(*m));
    amp_acquisition_init(&m->acq, &ops, 7);
    assert(!amp_acquisition_arm(&m->acq, &c, 0));
}
static int poll(struct model *m, rt_u64 cycle, rt_u32 phase, int high)
{
    struct amp_observation now = {cycle * 3750U + phase, phase, 3750U, high};
    return amp_acquisition_poll(&m->acq, &now);
}
static void one_cycle(struct model *m, rt_u32 cycle)
{
    /* 测试实际观测序列，既有高电平又有低沿；总线读取后的处理时间另由硬件帧算法限制。 */
    assert(!poll(m, cycle, 0, 1));
    assert(!poll(m, cycle, 150, 1));
    assert(!poll(m, cycle, 200, 0));
    assert(!poll(m, cycle, 3700, 0));
}
static void qualify(struct model *m)
{
    rt_u32 cycle;
    assert(!poll(m, 0, 3700, 0));
    for (cycle = 1; cycle <= AMP_QUALIFY_FRAMES; cycle++)
        one_cycle(m, cycle);
    assert(m->acq.qualified == 128 && m->acq.state == RT_STATE_RUNNING);
    assert(m->reads == 128 && !m->publishes);
}

int main(void)
{
    struct model m;
    struct rt_acq_config_v1 c;
    rt_u32 i;
    unsigned cases = 0;
    init(&m, 31);
    qualify(&m);
    one_cycle(&m, 129);
    assert(m.publishes == 1 && rt_words_value(m.last.sample_sequence) == 1);
    assert(rt_words_value(m.last.sample_ticks) == 129U * 3750U);
    assert(m.last.valid_chip_mask == 31 && m.last.analog_count == 80);
    assert(m.last.digital_words == 1 && m.last.digital[0] == 0xa55a && !m.last.digital[1]);
    assert(m.last.digital_valid[0] == 0xffff && !m.last.digital_valid[1] && !m.last.digital_valid[2]);
    assert(!(m.last.flags & (RT_SAMPLE_ADC_ESTIMATED | RT_SAMPLE_EDGE_CAPTURED | RT_SAMPLE_UTC_VALID)));
    for (i = 0; i < 80; i++)
        assert(m.last.analog[i] == (rt_s16)(-12000 + (int)(i / 16) * 1000 + (int)(i % 16)));
    assert(m.last.payload_crc32 == rt_crc32(m.last.analog, 176));
    cases++;
    /* 所选单片可在任意槽位；未选片不能挤掉原通道编号。 */
    for (rt_u32 chip = 0; chip < 5; chip++) {
        init(&m, 1U << chip);
        qualify(&m);
        one_cycle(&m, 129);
        assert(m.last.valid_chip_mask == (1U << chip));
        for (i = 0; i < 80; i++)
            assert(m.last.analog[i] == (i / 16 == chip ?
                (rt_s16)(-12000 + (int)chip * 1000 + (int)(i % 16)) : 0));
        cases++;
    }
    init(&m, 31);
    assert(!poll(&m, 0, 3700, 0));
    assert(!poll(&m, 1, 0, 0));
    assert(poll(&m, 1, 500, 0) == AMP_EBUSY_EDGE);
    assert(!m.reads && !m.publishes && m.stops == 1);
    cases++;
    init(&m, 31);
    assert(!poll(&m, 0, 3700, 0));
    assert(!poll(&m, 1, 100, 1));
    assert(poll(&m, 1, 500, 1) == AMP_EBUSY_EDGE);
    assert(!m.reads && m.acq.busy_timeouts == 1);
    cases++;
    init(&m, 31);
    assert(!poll(&m, 0, 3700, 0));
    assert(poll(&m, 2, 100, 1) == AMP_EMISSED_CYCLE);
    assert(m.acq.missed_cycles == 1 && !m.reads);
    cases++;
    init(&m, 31);
    assert(!poll(&m, 0, 3700, 0));
    assert(!poll(&m, 1, 100, 1));
    assert(!poll(&m, 1, 200, 0));
    assert(m.reads == 1 && m.acq.qualified == 1 && !m.acq.errors);
    cases++;
    init(&m, 31);
    m.read_error = RT_LBC_ETIMEOUT;
    assert(!poll(&m, 0, 3700, 0));
    assert(!poll(&m, 1, 150, 1));
    assert(poll(&m, 1, 200, 0) == RT_LBC_ETIMEOUT);
    assert(m.acq.read_errors == 1 && !m.publishes);
    cases++;
    init(&m, 31);
    qualify(&m);
    m.publish_error = RT_FULL;
    assert(!poll(&m, 129, 150, 1));
    assert(poll(&m, 129, 200, 0) == RT_FULL);
    assert(m.acq.faults & RT_FAULT_RING_FULL);
    cases++;
    init(&m, 31);
    assert(poll(&m, 6500, 0, 1) == AMP_EHOST_LEASE);
    assert(m.stops == 1 && !m.reads);
    cases++;
    init(&m, 31);
    qualify(&m);
    assert(!amp_acquisition_stop(&m.acq));
    assert(poll(&m, 129, 200, 0) == RT_REPLY_BAD_STATE);
    assert(!m.publishes);
    c = config(31);
    assert(!amp_acquisition_arm(&m.acq, &c, 129U * 3750U));
    assert(!m.acq.qualified && m.acq.state == RT_STATE_ARMED);
    cases++;
    init(&m, 31);
    m.stop_error = RT_LBC_EIO;
    assert(amp_acquisition_stop(&m.acq) == RT_LBC_EIO);
    assert(m.acq.state == RT_STATE_FAULT && m.acq.armed);
    cases++;
    init(&m, 31);
    assert(!amp_acquisition_stop(&m.acq));
    c = config(31); c.crc32 ^= 1;
    assert(amp_acquisition_arm(&m.acq, &c, 0) == RT_REPLY_BAD_CRC);
    c = config(3);
    assert(amp_acquisition_arm(&m.acq, &c, 0) == RT_REPLY_BAD_CONFIG);
    assert(m.reads == 0);
    cases++;
    /* 真实PWM尾部先升BUSY，随后回绕，不能误报成上次转换后的异常。 */
    init(&m, 31);
    assert(!poll(&m, 0, 3700, 0));
    one_cycle(&m, 1);
    assert(!poll(&m, 1, 3725, 1));
    one_cycle(&m, 2);
    assert(m.reads == 2 && m.acq.qualified == 2 && !m.acq.errors);
    cases++;
    init(&m, 31);
    assert(!poll(&m, 0, 3700, 0));
    assert(!poll(&m, 1, 150, 1));
    assert(!poll(&m, 1, 200, 0));
    assert(poll(&m, 1, 500, 1) == AMP_EBUSY_EDGE);
    assert(m.reads == 1 && m.acq.errors == 1);
    cases++;
    printf("{\"cases\":%u,\"adc_hardware_tested\":false,\"result\":\"passed\"}\n", cases);
    return 0;
}
