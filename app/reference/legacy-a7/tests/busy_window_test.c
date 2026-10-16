#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "adc_frame.h"

struct model { rt_u64 ns; rt_u32 period, after, reads, selected, step; int busy; };
static rt_u64 clock_ns(void *context) { return ((struct model *)context)->ns; }
static int phase(void *context, rt_u32 *value, rt_u32 *period)
{
    struct model *m = context; *value = m->after; *period = m->period; return 0;
}
static int busy(void *context) { return ((struct model *)context)->busy; }
static int select_chip(void *context, rt_u32 chip)
{
    ((struct model *)context)->selected = chip; return 0;
}
static int read_selected(void *context, rt_u16 *data, rt_u32 words)
{
    struct model *m = context; rt_u32 i;
    assert(words == 16); m->reads++; m->ns += m->step;
    for (i = 0; i < words; i++) data[i] = (rt_u16)(m->selected * 16U + i);
    return 0;
}
int main(void)
{
    struct model m;
    struct rt_adc_frame_ops ops = {.context=&m, .phase=phase, .busy=busy,
        .clock_ns=clock_ns, .select_chip=select_chip, .read_selected=read_selected,
        .start_from_busy_event=1};
    struct rt_adc_busy_event event;
    rt_s16 analog[80]; rt_u32 mask, before, i; rt_u64 ns;
    unsigned scenario;
    for (scenario = 0; scenario < 9; scenario++) {
        event = (struct rt_adc_busy_event){.phase=120, .clock_ns=1000};
        m = (struct model){.ns=1000, .period=1875, .after=1000, .step=2000};
        for (i = 0; i < 80; i++) analog[i] = -1234;
        switch (scenario) {
        case 1: m.ns = 1001; break; /* 1ns也必须向保守方向进位1tick。 */
        case 2: event.phase = 747; m.ns = 5000; break; /* 原1128tick总窗，setup96tick已耗用。 */
        case 3: event.phase = 748; m.ns = 5000; break; /* 事件再晚1tick须在首RD前拒绝。 */
        case 4: m.ns = 999; break;
        case 5: m.ns = 81000; break; /* 旧周期事件不能复用。 */
        case 6: m.busy = 1; break;
        case 7: m.step = 20000; break; /* 回绕后相位仍增大，单调时间仍须拒绝。 */
        case 8: m.period = 1250; break;
        }
        int result = rt_adc_read_frame_on_busy(&ops, analog, 1875, 31, &event, &mask, &before, &ns);
        if (scenario < 3) {
            assert(!result && mask == 31 && m.reads == 5);
            assert(before == (scenario == 0 ? 120U : scenario == 1 ? 121U : 843U));
            for (i = 0; i < 80; i++) assert(analog[i] == (rt_s16)i);
        } else {
            assert(result && !mask);
            if (scenario <= 6) assert(!m.reads);
            for (i = 0; i < 80; i++) assert(analog[i] == -1234);
        }
    }
    ops.clock_ticks = clock_ns;
    for (scenario = 0; scenario < 12; scenario++) {
        event = (struct rt_adc_busy_event){.phase=120, .clock_ns=~(rt_u64)0, .monotonic_ticks=1000};
        m = (struct model){.ns=1000, .period=1875, .after=1000, .step=48};
        for (i = 0; i < 80; i++) analog[i] = -1234;
        switch (scenario) {
        case 1: m.ns = 1001; break;
        case 2: event.phase=747; m.ns=1096; break;
        case 3: event.phase=748; m.ns=1096; break;
        case 4: m.ns=1097; break; /* setup超1tick仍拒绝，不能省去首RD保护。 */
        case 5: m.ns=999; break;
        case 6: m.ns=2875; break;
        case 7: m.busy=1; break;
        case 8: m.ns=1096; m.step=220; break; /* 总预算包括已经耗用的setup。 */
        case 9: m.step=400; break;
        case 10: m.period=1250; break;
        case 11: m.ns=~(rt_u64)0; break;
        }
        int result = rt_adc_read_frame_on_busy(&ops, analog, 1875, 31, &event, &mask, &before, &ns);
        if (scenario < 3) {
            assert(!result && mask==31 && m.reads==5 && ns==10000);
            assert(before==(scenario==0 ? 120U : scenario==1 ? 121U : 843U));
            for (i=0; i<80; i++) assert(analog[i]==(rt_s16)i);
        } else {
            assert(result && !mask);
            if (scenario<=7 || scenario==11) assert(!m.reads);
            if (scenario==8) assert(result==RT_ADC_EREAD_BUDGET);
            for (i=0; i<80; i++) assert(analog[i]==-1234);
        }
    }
    puts("BUSY bounded start: 9 ns and 12 native tick timing/atomic delivery scenarios passed");
    return 0;
}
