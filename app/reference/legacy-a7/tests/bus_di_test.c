#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "acquisition.h"

static unsigned stops, reads, published;
static int di_error;
static struct rt_sample_v1 last;
static int prepare(void *ctx, const struct rt_adc_register_plan *p) { (void)ctx; (void)p; return 0; }
static int start(void *ctx, rt_u32 p) { (void)ctx; (void)p; return 0; }
static int stop(void *ctx) { (void)ctx; stops++; return 0; }
static int adc(void *ctx, const struct rt_adc_busy_event *e, rt_s16 *a, rt_u32 *m, rt_u64 *n)
{ (void)ctx; (void)e; (void)a; (void)m; (void)n; assert(!"DI模式不得读取ADC"); return -1; }
static int digital(void *ctx, rt_u32 *raw, rt_u32 *ticks)
{ (void)ctx; reads++; *raw = 0x69f0a55a; *ticks = 72; return di_error; }
static int publish(void *ctx, const struct rt_sample_v1 *s) { (void)ctx; last = *s; published++; return 0; }
int main(void)
{
    struct amp_acquisition a;
    struct amp_acquisition_ops ops = {.prepare=prepare,.start=start,.stop=stop,.read=adc,.publish=publish,.read_digital=digital};
    struct rt_acq_config_v1 c = {.abi_version=RT_ABI_V1,.bytes=sizeof(c),.generation=1,.initial_period_ticks=1875,.enabled_chip_mask=31,.samples_per_cycle=256};
    struct amp_observation now = {.ticks=1000};
    memset(c.range_code,3,sizeof(c.range_code)); c.crc32=rt_crc32(&c,sizeof(c));
    amp_acquisition_init(&a,&ops,7);
    assert(!amp_acquisition_arm(&a,&c,1000));
    assert(!amp_acquisition_poll(&a,&now));
    assert(a.state==RT_STATE_RUNNING && !a.qualified && !a.read_count && !a.falls);
    assert(reads==1 && published==1 && last.analog_count==0 && last.valid_chip_mask==0);
    assert(last.digital_words==1 && last.digital[0]==0xa55a && !last.digital[1]);
    assert(last.digital_valid[0]==0xffff && !last.digital_valid[1]);
    assert(!(last.flags&(RT_SAMPLE_ADC_VALID|RT_SAMPLE_ADC_ESTIMATED)));
    assert(last.payload_crc32==rt_crc32(last.analog,176));
    now.ticks+=10;
    assert(!amp_acquisition_poll(&a,&now));
    assert(reads==1 && a.di_due_ticks==1000+AMP_BUS_DI_PERIOD_TICKS);
    /* 绝对期限保持不漂移；跨过的周期只计漏采，不补造样点。 */
    now.ticks=1000+AMP_BUS_DI_PERIOD_TICKS+3;
    assert(!amp_acquisition_poll(&a,&now));
    assert(reads==2 && published==2 && !a.di_missed_periods && !a.di_changes);
    assert(rt_words_value(last.sample_ticks)==now.ticks && last.period_ticks==AMP_BUS_DI_PERIOD_TICKS);
    now.ticks=1000+4U*AMP_BUS_DI_PERIOD_TICKS+5;
    assert(!amp_acquisition_poll(&a,&now));
    assert(reads==3 && published==3 && a.di_missed_periods==2);
    assert(a.di_due_ticks==1000+5U*AMP_BUS_DI_PERIOD_TICKS);
    now.ticks=a.di_due_ticks; di_error=-9;
    assert(amp_acquisition_poll(&a,&now)==-9);
    assert(a.di_errors==1 && !a.di_valid && !a.armed && stops==1);
    assert(published==3);
    di_error=0; amp_acquisition_init(&a,&ops,9);
    assert(!amp_acquisition_arm(&a,&c,1000));
    now.ticks=1000+AMP_HOST_LEASE_TICKS;
    assert(amp_acquisition_poll(&a,&now)==AMP_EHOST_LEASE && !a.armed);
    puts("{\"di_frame_crc_order_and_lease\":\"passed\",\"adc_frames\":0}");
    return 0;
}
