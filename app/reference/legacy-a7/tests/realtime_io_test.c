#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "realtime_io.h"
#include "rt_shared_ring.h"
#include "aux_boot_time.h"

/* 模型明确实现INT_FLAG W1C/重复START/末字节NACK，能识别错误的寄存器推进方式。 */
struct cell { rt_u32 address,value; };
static struct cell registers[300];
static unsigned int cells, writes;
static rt_u64 clock_ticks;
static struct amp_rtio_snapshot latest;
static struct amp_rtio_command queued;
static rt_u8 rtc[16], temperature[2]={25,128};
static unsigned int phase, address, pointer, nak_temperature, stalled;
#define TWI 0x02511000U
#define PWM 0x020a0000U
static rt_u32 *reg(rt_u32 address_value)
{
    for(unsigned int i=0;i<cells;i++) if(registers[i].address==address_value) return &registers[i].value;
    assert(cells<300); registers[cells].address=address_value; return &registers[cells++].value;
}
rt_u64 amp_ticks(void) { return ++clock_ticks; }
rt_u32 amp_rtio_test_read(rt_u32 a) { clock_ticks++; return *reg(a); }
static void ready(rt_u32 status)
{
    *reg(TWI+0x10U)=status; *reg(TWI+0xcU)=0x48U;
}
void amp_rtio_test_write(rt_u32 a,rt_u32 value)
{
    clock_ticks++; writes++;
    if(a==TWI+0x18U) { phase=0; *reg(TWI+0x10U)=0xf8U; return; }
    if(a==TWI+0xcU) {
        if(!value) { *reg(a)=0; return; }
        if(stalled) { *reg(a)=value&~8U; return; }
        if(value&0x10U) { *reg(a)=value; phase=9; return; }
        if(value&0x20U) { ready(phase?0x10U:8U); phase=1; return; }
        if(!(value&8U)) { *reg(a)=value|(*reg(a)&8U); return; }
        switch(phase) {
        case 0: *reg(a)=value&~8U; return;
        case 9: phase=0; *reg(TWI+0x10U)=0xf8U; *reg(a)=0x40U; return;
        case 1:
            address=*reg(TWI+8U)>>1U;
            if(address==0x48U && nak_temperature) { ready(0x20U); return; }
            if(*reg(TWI+8U)&1U) { phase=4; ready(0x40U); }
            else { phase=2; ready(0x18U); }
            return;
        case 2: pointer=address==0x32U?(*reg(TWI+8U)>>4U):*reg(TWI+8U); phase=3; ready(0x28U); return;
        case 3: assert(address==0x32U); rtc[pointer++&15U]=(rt_u8)*reg(TWI+8U); ready(0x28U); return;
        case 4:
            *reg(TWI+8U)=address==0x32U?rtc[pointer++&15U]:temperature[pointer++&1U];
            ready(value&4U?0x50U:0x58U); return;
        default:assert(0);
        }
    }
    if((a>=PWM+0x1000U && a<PWM+0x5000U && (a&0xfffU)==12U) ||
       a==0x02087038U || a==0x02089038U) *reg(a)&=~value;
    else *reg(a)=value;
}
void amp_rtio_test_snapshot(const struct amp_rtio_snapshot *s) { latest=*s; }
int amp_rtio_test_command(struct amp_rtio_command *c) { *c=queued; return queued.request_id!=0; }
static void run(unsigned int steps)
{
    for(unsigned int i=0;i<steps;i++) { clock_ticks+=2400; amp_rtio_service(clock_ticks); }
    amp_rtio_publish(1);
}
static void calendar_tests(void)
{
    rt_u64 utc;
    rt_u8 r[16]={0};
    struct amp_rtio_command c={.year=2024,.month=2,.day=29,.hour=23,.minute=59,.second=59,.weekday=4};
    r[4]=1;r[5]=1;r[14]=r[15]=0x20;
    assert(!amp_rtio_decode_rtc(r,&utc) && utc==946684800ULL);
    assert(!amp_rtio_encode_rtc(&c,r));
    assert(!amp_rtio_decode_rtc(r,&utc) && utc==1709251199ULL);
    c.year=2023; assert(amp_rtio_encode_rtc(&c,r)==-22);
    c.year=2024;c.weekday=7;assert(amp_rtio_encode_rtc(&c,r)==-22);
    r[15]=0x30;assert(amp_rtio_decode_rtc(r,&utc)==-22);
    r[15]=0x20;r[1]=0x6a;assert(amp_rtio_decode_rtc(r,&utc)==-22);
    r[1]=0;r[14]=0;r[2]=0x32; assert(!amp_rtio_decode_rtc(r,&utc));
    r[2]=0;assert(amp_rtio_decode_rtc(r,&utc)==-22);
}
static void edge(unsigned int slot,rt_u32 period,rt_u64 elapsed)
{
    const unsigned int channels[]={2,7,0,5};
    rt_u32 ch=channels[slot],g=PWM+(ch/2+1)*0x1000;
    clock_ticks+=elapsed;
    *reg(g+12)=1U<<((ch&1)*2);
    *reg(g+0x138+(ch&1)*0x100)=period-1;
    amp_rtio_irq(clock_ticks);amp_rtio_publish(1);
}
static void boot_time_tests(void)
{
    rt_u64 utc=0;
    struct amp_rtio_snapshot s={.magic=AMP_RTIO_MAGIC,.version=AMP_RTIO_VERSION,
        .bytes=sizeof(s),.epoch=17,.active=1,.flags=AMP_RTIO_RTC_VALID};
    s.ticks=rt_words_make(24000000);s.observed_ticks=rt_words_make(24024000);
    s.rtc_ticks=rt_words_make(12000000);s.rtc_utc=rt_words_make(1760000000);
    assert(amp_aux_boot_utc(&s,17,0,&utc)==1 && utc==1760000000501000000ULL);
    assert(amp_aux_boot_utc(&s,17,1760000002,&utc)==0);
    s.rtc_error=-22;assert(amp_aux_boot_utc(&s,17,0,&utc)==2);
    assert(amp_aux_boot_utc(&s,17,1760000002,&utc)==0);
    s.rtc_error=0;s.flags=0;assert(amp_aux_boot_utc(&s,17,0,&utc)==2);
    s.flags=AMP_RTIO_RTC_VALID;s.rtc_ticks=rt_words_make(0);
    assert(amp_aux_boot_utc(&s,17,0,&utc)==2);
    assert(amp_aux_boot_utc(&s,18,0,&utc)==-71);
    s.observed_ticks=rt_words_make(31000000);assert(amp_aux_boot_utc(&s,17,1760000002,&utc)==-116);
}
int main(void)
{
    calendar_tests();
    boot_time_tests();
    rtc[4]=1;rtc[5]=1;rtc[6]=0x26;rtc[14]=0x20;rtc[15]=0x20;
    *reg(TWI+0x10)=0xf8;
    *reg(0x03401830)=0x12345678; *reg(0x03401430)=0x11223344;
    *reg(PWM+0x3c)=0x80; *reg(PWM+0x24)=0x10;
    *reg(PWM+0x3020)=0x12340000; *reg(PWM+0x3100)=0x4567;
    /* 期望地址独立抄自SDK布局，避免实现与模型共用同一错误的bank公式。
     * PA4 BUSY/PA5 RESET和PB13调压必须原样，GPIO全局bank表禁止写入。 */
    *reg(0x03604000)=0x4080; *reg(0x03604004)=0x4100; *reg(0x03604028)=0x4580;
    *reg(0x03604080)=0x00100000; *reg(0x03604104)=0x00500000;
    amp_rtio_init(9);assert(!amp_rtio_start());
    assert(*reg(0x03604000)==0x4080 && *reg(0x03604004)==0x4100);
    assert(*reg(0x03604080)==0x00102222 && *reg(0x03604084)==0x222);
    assert(*reg(0x03604104)==0x05505000 && *reg(0x03604584)==0x6000);
    assert(*reg(0x03604100)==0x00220000);
    assert(*reg(PWM+0x24)==0xb4);
    run(180);
    assert(latest.active && latest.rtc_error==0 && latest.rtc_reads);
    assert(latest.temperature_error==0 && latest.temperature_milli_c==25500);
    assert(latest.flags&AMP_RTIO_RTC_VALID); assert(latest.capture[2].error==-16);
    assert(rtc[14]==0x24); assert(*reg(0x02087028)==0 && *reg(0x02089028)==0);
    *reg(0x02087038)=0x70;*reg(0x02089038)=15;
    for(unsigned int i=0;i<7;i++) *reg((i<4?0x02089080:0x02087080)+i*4)=100+i;
    run(20);assert(latest.gpadc_valid==127 && latest.gpadc_raw[6]==106);
    rt_u64 old_channel=rt_words_value(latest.gpadc_ticks[6]);
    *reg(0x02087038)=1U<<4;run(20);
    assert(rt_words_value(latest.gpadc_ticks[4])>old_channel);
    assert(rt_words_value(latest.gpadc_ticks[6])==old_channel);
    edge(0,1500000,24000000); assert(latest.capture[0].error==-61);
    edge(0,1500000,24000000); assert(!latest.capture[0].error);
    edge(3,30000,480000); edge(3,30000,480000);assert(!latest.capture[3].error);
    edge(3,30000,960000);assert(latest.capture[3].error==-34);
    nak_temperature=1;run(11000);assert(latest.temperature_error==-6 && latest.rtc_error==0);
    nak_temperature=0;run(11000);assert(latest.temperature_error==0);
    queued=(struct amp_rtio_command){.version=1,.bytes=sizeof(queued),.request_id=1,.epoch=8,.operation=1,.year=2026,.month=10,.day=16,.weekday=5};
    run(10);assert(latest.rtc_request_id==1 && latest.rtc_request_result==-71);
    queued.epoch=9;queued.request_id=2;queued.crc32=rt_crc32(&queued,offsetof(struct amp_rtio_command,crc32));
    run(220);assert(latest.rtc_request_id==2 && latest.rtc_request_result==0 && rtc[6]==0x26 && rtc[5]==0x10);
    amp_rtio_irq(amp_ticks());amp_rtio_publish(1);
    assert(latest.unexpected_capture==1 && (latest.flags&AMP_RTIO_CAPTURE_FAULT));
    assert(latest.capture[0].error==-5 && latest.capture[3].error==-5);
    assert(*reg(0x03401184)==(1U<<19));
    *reg(0x03401830)=(*reg(0x03401830)&0xff000000U)|0xabcdefU;
    assert(!amp_rtio_stop());amp_rtio_publish(1);
    assert(!latest.active && !latest.gpadc_valid);
    assert(*reg(0x03401830)==0x12abcdefU && *reg(0x03401430)==0x11223344);
    assert(*reg(PWM+0x3c)==0x80 && *reg(PWM+0x3020)==0x12340000 && *reg(PWM+0x3100)==0x4567);
    assert(*reg(PWM+0x24)==0x10);
    assert(rtc[14]==0x20 && *reg(0x03604000)==0x4080 && *reg(0x03604004)==0x4100);
    assert(*reg(0x03604080)==0x00100000 && *reg(0x03604084)==0);
    assert(*reg(0x03604104)==0x00500000 && *reg(0x03604584)==0);
    /* 控制器冲突在第一笔硬件写之前拒绝；故障日期和超时不被改成正常。 */
    amp_rtio_init(10);*reg(TWI+0xc)=0x80;unsigned int before=writes;
    assert(amp_rtio_start()==-16 && before==writes);
    *reg(TWI+0xc)=0;assert(!amp_rtio_start());rtc[15]=0;run(150);
    assert(latest.rtc_error==-22 && !(latest.flags&AMP_RTIO_RTC_VALID));
    stalled=1;run(12000);assert(latest.rtc_error==-110);
    stalled=0;assert(!amp_rtio_stop());
    *reg(0x03604000)=0;before=writes;
    assert(amp_rtio_start()==-5 && before==writes);
    puts("RTIO calendar, W1C I2C, NAK/timeout, mailbox CRC/epoch, capture qualification, GPADC, ownership rollback passed");
    return 0;
}
