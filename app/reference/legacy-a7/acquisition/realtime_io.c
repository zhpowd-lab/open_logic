/* SPDX-License-Identifier: GPL-2.0 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
#include "realtime_io.h"
#include "rt_shared_ring.h"

static int leap(rt_u32 year) { return !(year % 4U) && (year % 100U || !(year % 400U)); }
static rt_u32 days_in_month(rt_u32 year, rt_u32 month)
{
    static const rt_u8 days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month-1U] + (month == 2U && leap(year));
}
static int date_valid(rt_u32 y, rt_u32 m, rt_u32 d, rt_u32 h, rt_u32 n, rt_u32 s)
{
    return y >= 2000U && y <= 2099U && m >= 1U && m <= 12U && d >= 1U &&
        d <= days_in_month(y,m) && h < 24U && n < 60U && s < 60U;
}
static int bcd(rt_u8 value)
{
    return (value & 15U) > 9U || (value >> 4U) > 9U ? -1 : (int)((value >> 4U)*10U + (value & 15U));
}
int amp_rtio_decode_rtc(const rt_u8 r[16], rt_u64 *utc)
{
    int y, m, d, h, n, s;
    rt_u64 days = 10957U;
    rt_u32 i;
    if (!r || !utc) return -22;
    /* RX8025的XST=0或PON=1表示日期无效，不能靠成功读出数字掩盖。 */
    if (!(r[15]&0x20U) || (r[15]&0x10U)) return -22;
    s=bcd(r[0]&0x7fU); n=bcd(r[1]&0x7fU);
    h=bcd(r[2] & ((r[14]&0x20U) ? 0x3fU : 0x1fU));
    if (!(r[14]&0x20U)) {
        if (h < 1 || h > 12) return -22;
        h=h%12 + ((r[2]&0x20U) ? 12 : 0);
    }
    d=bcd(r[4]&0x3fU); m=bcd(r[5]&0x1fU); y=bcd(r[6]);
    if (y<0 || m<0 || d<0 || h<0 || n<0 || s<0 || !date_valid((rt_u32)y+2000U,m,d,h,n,s)) return -22;
    for (i=2000U;i<(rt_u32)y+2000U;i++) days+=365U+leap(i);
    for (i=1;i<(rt_u32)m;i++) days+=days_in_month((rt_u32)y+2000U,i);
    *utc=((days+(rt_u32)d-1U)*24U+(rt_u32)h)*3600U+(rt_u32)n*60U+(rt_u32)s;
    return 0;
}
int amp_rtio_encode_rtc(const struct amp_rtio_command *c, rt_u8 raw[7])
{
    rt_u32 fields[7], i;
    if (!c || !raw || !date_valid(c->year,c->month,c->day,c->hour,c->minute,c->second) || c->weekday>6U) return -22;
    fields[0]=c->second; fields[1]=c->minute; fields[2]=c->hour;
    fields[3]=c->weekday; fields[4]=c->day; fields[5]=c->month; fields[6]=c->year-2000U;
    for (i=0;i<7;i++) raw[i]=(rt_u8)((fields[i]/10U)*16U+fields[i]%10U);
    return 0;
}

#if AMP_REALTIME_IO && !defined(__KERNEL__)
#define GPIO 0x03604000U
#define PWMCS 0x020a0000U
#define TWI 0x02511000U
#define GPADC0 0x02087000U
#define GPADC2 0x02089000U
#define RTP 0x0208c000U
#define GICD 0x03401000U
#define STATUS 0x442f0000U
#ifdef AMP_RTIO_HOST_TEST
extern rt_u32 amp_rtio_test_read(rt_u32 address);
extern void amp_rtio_test_write(rt_u32 address,rt_u32 value);
extern rt_u64 amp_ticks(void);
extern void amp_rtio_test_snapshot(const struct amp_rtio_snapshot *snapshot);
extern int amp_rtio_test_command(struct amp_rtio_command *command);
#define rd(a) amp_rtio_test_read(a)
#define wr(a,v) amp_rtio_test_write(a,v)
static rt_u32 irq_save(void) { return 0; }
static void irq_restore(rt_u32 flags) { (void)flags; }
#else
extern rt_u64 amp_ticks(void);
static rt_u32 rd(rt_u32 address) { return *(volatile rt_u32 *)(uintptr_t)address; }
static void wr(rt_u32 address, rt_u32 value)
{
    *(volatile rt_u32 *)(uintptr_t)address=value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static rt_u32 irq_save(void)
{
    rt_u32 value; __asm__ volatile("mrs %0,cpsr\ncpsid i" : "=r"(value) :: "memory"); return value;
}
static void irq_restore(rt_u32 value) { __asm__ volatile("msr cpsr_c,%0" :: "r"(value) : "memory"); }
#endif
static struct amp_rtio_snapshot local;
static rt_u32 saved_capture[3][3], saved_global, saved_input_mux, gic_target, gic_priority;
static const rt_u32 pin_ports[] = {1,1,10,0,0,0,0,0,0,0,1,1};
static const rt_u32 pin_numbers[] = {11,14,11,0,1,2,3,8,9,10,4,5};
static rt_u32 saved_pins[12], saved_analog[2][7];
static const rt_u32 analog_offsets[] = {0,4,8,0xc,0x20,0x24,0x28};
static rt_u32 capture_channels[3] = {2U,7U,5U};
static rt_u32 capture_slots[3] = {0U,1U,3U};
static rt_u64 next_rtc, next_analog;
/* 一个后台调用只推进一次寄存器状态，I2C等待分散到完整ADC帧后的空隙。 */
struct twi_transfer {
    rt_u32 state, address, reg, length, position, writing;
    rt_u8 data[16];
    rt_u64 deadline;
    int result;
};
static struct twi_transfer bus;
static rt_u32 rtc_stage, last_request, saved_twi_clock, saved_twi_control, saved_twi_efr;
static rt_u32 rtc_ctrl_saved, rtc_ctrl_changed;
static rt_u32 saved_rtp_mode;
static rt_u8 rtc_raw[16], write_date[7];
static struct amp_rtio_command request;
/* sun8iw22的首bank在0x80，间隔也是0x80；0x00是bank地址表，绝不能当PA复用寄存器。 */
static rt_u32 pin_address(rt_u32 port, rt_u32 pin) { return GPIO+(port+1U)*0x80U+(pin/8U)*4U; }
static void mux(rt_u32 port, rt_u32 pin, rt_u32 value)
{
    rt_u32 a=pin_address(port,pin), shift=(pin%8U)*4U;
    wr(a,(rd(a)&~(15U<<shift))|(value<<shift));
}
static rt_u32 group(rt_u32 channel) { return (channel/2U+1U)*0x1000U; }
static void capture_enable(rt_u32 ch, int enabled)
{
    rt_u32 g=PWMCS+group(ch), bit=1U<<(ch&1U), ibit=1U<<((ch&1U)*2U);
    wr(g+8U,rd(g+8U)&~ibit); wr(g+0x38U,rd(g+0x38U)&~bit); wr(g+12U,ibit);
    if (enabled) { wr(g+8U,rd(g+8U)|ibit); wr(g+0x38U,rd(g+0x38U)|bit); }
}
static void i2c_begin(rt_u32 address, rt_u32 reg, rt_u32 length, const rt_u8 *data, rt_u64 now)
{
    memset(&bus,0,sizeof(bus)); bus.address=address; bus.reg=reg; bus.length=length;
    bus.writing=data!=0; if(data) memcpy(bus.data,data,length);
    bus.state=1; bus.result=-115; bus.deadline=now+2400000U;
    wr(TWI+0xcU,0x68U);
}
static void i2c_stop(int result)
{
    /* STOP先置位，再W1C推进状态；不能用写0清INT_FLAG。 */
    wr(TWI+0xcU,0x50U); wr(TWI+0xcU,0x48U); bus.result=result; bus.state=9;
}
static int i2c_step(rt_u64 now)
{
    rt_u32 status, control=rd(TWI+0xcU);
    if (now>bus.deadline) {
        /* 超时停止总线并复位本次独占控制器；不循环重试RTC写请求。 */
        wr(TWI+0x18U,1U); wr(TWI+0xcU,0x40U);
        bus.state=0; bus.result=-110; return 1;
    }
    if (bus.state==9) {
        if ((control&0x10U) || rd(TWI+0x10U)!=0xf8U) return 0;
        bus.state=0; return 1;
    }
    if (!(control&8U)) return 0;
    status=rd(TWI+0x10U);
    switch(bus.state) {
    case 1:
        if(status!=8U) break;
        wr(TWI+8U,bus.address<<1U); wr(TWI+0xcU,0x48U); bus.state=2; return 0;
    case 2:
        if(status!=0x18U) break;
        wr(TWI+8U,bus.reg); wr(TWI+0xcU,0x48U); bus.state=3; return 0;
    case 3:
        if(status!=0x28U) break;
        if(bus.writing) { wr(TWI+8U,bus.data[0]); bus.position=1; wr(TWI+0xcU,0x48U); bus.state=7; }
        else { wr(TWI+0xcU,0x68U); bus.state=4; }
        return 0;
    case 4:
        if(status!=0x10U) break;
        wr(TWI+8U,(bus.address<<1U)|1U); wr(TWI+0xcU,0x48U); bus.state=5; return 0;
    case 5:
        if(status!=0x40U) break;
        wr(TWI+0xcU,bus.length>1U?0x4cU:0x48U); bus.state=6; return 0;
    case 6:
        if(status!=(bus.position+1U==bus.length?0x58U:0x50U)) break;
        bus.data[bus.position++]=(rt_u8)rd(TWI+8U);
        if(bus.position==bus.length) i2c_stop(0);
        else wr(TWI+0xcU,bus.position+1U<bus.length?0x4cU:0x48U);
        return 0;
    case 7:
        if(status!=0x28U) break;
        if(bus.position==bus.length) i2c_stop(0);
        else { wr(TWI+8U,bus.data[bus.position++]); wr(TWI+0xcU,0x48U); }
        return 0;
    default:break;
    }
    i2c_stop(status==0x20U || status==0x48U ? -6 : -5); return 0;
}
void amp_rtio_init(rt_u32 epoch)
{
    memset(&local,0,sizeof(local)); local.magic=AMP_RTIO_MAGIC; local.version=AMP_RTIO_VERSION;
    local.bytes=sizeof(local); local.epoch=epoch; local.rtc_error=local.temperature_error=-61;
    local.flags=AMP_RTIO_FREQ0_CONFLICT;
    for(rt_u32 i=0;i<4;i++) local.capture[i].error=i==2U?-16:-61;
    bus.state=rtc_stage=last_request=rtc_ctrl_changed=0;
}
int amp_rtio_start(void)
{
    rt_u32 i, j, shift=(AMP_RTIO_IRQ&3U)*8U;
    if(local.active) return -16;
    /* 不支持被启动程序重定位的bank；读回核对后才允许第一次写。 */
    if((rd(GPIO)&0xffffU)!=0x4080U || (rd(GPIO+4U)&0xffffU)!=0x4100U ||
       (rd(GPIO+0x28U)&0xffffU)!=0x4580U) return -5;
    /* 所有冲突先验证，失败时不留半配置。PB13调压输出绝不切换为输入。 */
    for(i=0;i<3;i++) {
        rt_u32 ch=capture_channels[i], g=PWMCS+group(ch);
        if ((rd(g+0x38U)&3U) || rd(g+8U) || (rd(g+0x20U)&0x180U)) return -16;
    }
    for(i=0;i<2;i++) {
        rt_u32 base=i?GPADC2:GPADC0;
        if((rd(base+4U)&(1U<<16U)) || rd(base+0xcU)&0x30000U ||
           rd(base+0x20U) || rd(base+0x24U) || rd(base+0x28U)) return -16;
        for(j=0;j<7;j++) saved_analog[i][j]=rd(base+analog_offsets[j]);
    }
    if(rd(TWI+0xcU)&0xb0U) return -16;
    for(i=0;i<12;i++) saved_pins[i]=rd(pin_address(pin_ports[i],pin_numbers[i]));
    saved_global=rd(PWMCS+0x3cU);
    saved_input_mux=rd(PWMCS+0x24U);
    for(i=0;i<3;i++) {
        rt_u32 ch=capture_channels[i], g=PWMCS+group(ch);
        saved_capture[i][0]=rd(g+0x3cU); saved_capture[i][1]=rd(g+0x40U);
        saved_capture[i][2]=rd(g+0x134U+(ch&1U)*0x100U);
    }
    gic_target=(rd(GICD+0x800U+(AMP_RTIO_IRQ&~3U))>>shift)&255U;
    gic_priority=(rd(GICD+0x400U+(AMP_RTIO_IRQ&~3U))>>shift)&255U;
    saved_twi_clock=rd(TWI+0x14U); saved_twi_control=rd(TWI+0xcU);
    saved_twi_efr=rd(TWI+0x1cU);
    saved_rtp_mode=rd(RTP+4U)&16U;
    wr(RTP+4U,rd(RTP+4U)|16U);
    wr(PWMCS+0x3cU,saved_global|14U);
    /* SDK的输入通路另有独立门控；仅开ch2/5/7，保留调压及其它输出。 */
    wr(PWMCS+0x24U,saved_input_mux|0xa4U);
    for(i=0;i<3;i++) {
        rt_u32 ch=capture_channels[i], g=PWMCS+group(ch);
        /* 捕获分频独立于输出PCCR，不能改CPU调压的ch4输出时钟。 */
        wr(g+0x3cU,4U); wr(g+0x40U,rd(g+0x40U)|(1U<<(ch&1U)));
        wr(g+0x134U+(ch&1U)*0x100U,0x1500U);
    }
    mux(1,11,5); mux(1,14,5); mux(10,11,6);
    wr(GICD+0x800U+(AMP_RTIO_IRQ&~3U),
       (rd(GICD+0x800U+(AMP_RTIO_IRQ&~3U))&~(255U<<shift))|(8U<<shift));
    wr(GICD+0x400U+(AMP_RTIO_IRQ&~3U),
       (rd(GICD+0x400U+(AMP_RTIO_IRQ&~3U))&~(255U<<shift))|(0xa0U<<shift));
    wr(GICD+0x280U+(AMP_RTIO_IRQ/32U)*4U,1U<<(AMP_RTIO_IRQ%32U));
    for(i=0;i<4;i++) mux(0,i,2);
    for(i=8;i<11;i++) mux(0,i,2);
    for(i=0;i<2;i++) {
        rt_u32 base=i?GPADC2:GPADC0, mask=i?15U:0x70U;
        /* 轮询新数据pending，不向Linux投递GPADC中断。 */
        wr(base+4U,0); wr(base+0xcU,0); wr(base+0x20U,0); wr(base+0x24U,0); wr(base+0x28U,0);
        wr(base+0x38U,mask); wr(base,((24000U-1U)<<16U)|47U); wr(base+8U,mask);
        wr(base+4U,(2U<<18U)|(1U<<23U)|(1U<<17U)|(1U<<16U));
    }
    mux(1,4,2); mux(1,5,2);
    /* Linux持有并核对24MHz APB；100kHz保留RX8025旧总线速率。 */
    wr(TWI+0x18U,1U); wr(TWI+0x14U,23U<<3U); wr(TWI+0x1cU,0); wr(TWI+0xcU,0x48U);
    local.active=1;
    for(i=0;i<3;i++) capture_enable(capture_channels[i],1);
    wr(GICD+0x100U+(AMP_RTIO_IRQ/32U)*4U,1U<<(AMP_RTIO_IRQ%32U));
    next_rtc=next_analog=amp_ticks(); return 0;
}
static int i2c_finish(void)
{
    /* ADC已撤源才允许有限等待STOP；无法确认空闲时保留所有权，不伪造释放。 */
    while(bus.state) { if(i2c_step(amp_ticks())) break; }
    if(rd(TWI+0x10U)!=0xf8U || (rd(TWI+0xcU)&0x30U)) return -16;
    return 0;
}
int amp_rtio_stop(void)
{
    rt_u32 i, j, shift=(AMP_RTIO_IRQ&3U)*8U;
    if(!local.active) return 0;
    wr(GICD+0x180U+(AMP_RTIO_IRQ/32U)*4U,1U<<(AMP_RTIO_IRQ%32U));
    for(i=0;i<3;i++) capture_enable(capture_channels[i],0);
    if(i2c_finish()) return -16;
    if(rtc_ctrl_changed) {
        rt_u8 ctrl=(rt_u8)((rtc_raw[14]&~7U)|(rtc_ctrl_saved&7U));
        i2c_begin(0x32,0xe0,1,&ctrl,amp_ticks());
        if(i2c_finish() || bus.result) return -16;
        rtc_ctrl_changed=0;
    }
    for(i=0;i<3;i++) {
        rt_u32 ch=capture_channels[i], g=PWMCS+group(ch), bit=1U<<(ch&1U);
        wr(g+0x3cU,saved_capture[i][0]);
        wr(g+0x40U,(rd(g+0x40U)&~bit)|(saved_capture[i][1]&bit));
        wr(g+0x134U+(ch&1U)*0x100U,saved_capture[i][2]);
    }
    wr(PWMCS+0x3cU,(rd(PWMCS+0x3cU)&~14U)|(saved_global&14U));
    wr(PWMCS+0x24U,(rd(PWMCS+0x24U)&~0xa4U)|(saved_input_mux&0xa4U));
    for(i=0;i<2;i++) {
        rt_u32 base=i?GPADC2:GPADC0;
        wr(base+4U,0); wr(base+0x38U,i?15U:0x70U);
        for(j=0;j<7;j++) wr(base+analog_offsets[j],saved_analog[i][j]);
    }
    wr(RTP+4U,(rd(RTP+4U)&~16U)|saved_rtp_mode);
    wr(TWI+0xcU,0); wr(TWI+0x14U,saved_twi_clock); wr(TWI+0x1cU,saved_twi_efr);
    wr(TWI+0xcU,saved_twi_control&0x44U);
    /* 只恢复自己的复用字段，不能覆盖相邻Linux GPIO或GIC中断字节。 */
    for(i=0;i<12;i++) {
        rt_u32 a=pin_address(pin_ports[i],pin_numbers[i]),m=15U<<((pin_numbers[i]&7U)*4U);
        wr(a,(rd(a)&~m)|(saved_pins[i]&m));
    }
    wr(GICD+0x280U+(AMP_RTIO_IRQ/32U)*4U,1U<<(AMP_RTIO_IRQ%32U));
    wr(GICD+0x800U+(AMP_RTIO_IRQ&~3U),(rd(GICD+0x800U+(AMP_RTIO_IRQ&~3U))&~(255U<<shift))|(gic_target<<shift));
    wr(GICD+0x400U+(AMP_RTIO_IRQ&~3U),(rd(GICD+0x400U+(AMP_RTIO_IRQ&~3U))&~(255U<<shift))|(gic_priority<<shift));
    local.active=0; local.gpadc_valid=0; bus.state=0;
    return 0;
}
void amp_rtio_irq(rt_u64 now)
{
    rt_u64 started=amp_ticks(); rt_u32 handled=0;
    if(!local.active) {
        wr(GICD+0x180U+(AMP_RTIO_IRQ/32U)*4U,1U<<(AMP_RTIO_IRQ%32U));
        return;
    }
    for(rt_u32 i=0;i<3;i++) {
        rt_u32 ch=capture_channels[i], g=PWMCS+group(ch),bit=1U<<((ch&1U)*2U), slot=capture_slots[i];
        if(!(rd(g+12U)&bit)) continue;
        rt_u32 period=(rd(g+0x138U+(ch&1U)*0x100U)&0x7fffffU)+1U;
        wr(g+12U,bit); handled++;
        struct amp_rtio_capture *c=&local.capture[slot];
        rt_u64 previous=rt_words_value(c->last_ticks),elapsed=now-previous;
        c->count=rt_words_make(rt_words_value(c->count)+1U); c->last_ticks=rt_words_make(now); c->period=period;
        /* 首沿没有完整间隔，硬件周期和单调时钟交叉核对后才给测频/PPS资格。 */
        c->error=!previous?-61:(slot<2U?
            (elapsed<19200000U || elapsed>28800000U || period<1200000U || period>1800000U?-34:0):
            (elapsed<400000U || elapsed>650000U || period<26785U || period>37500U?-34:0));
    }
    if(!handled) {
        /* Linux已隔离整个捕获IRQ；未知源只屏蔽本IRQ并锁存，不清其它通道寄存器。 */
        wr(GICD+0x180U+(AMP_RTIO_IRQ/32U)*4U,1U<<(AMP_RTIO_IRQ%32U));
        local.unexpected_capture++;
        local.flags|=AMP_RTIO_CAPTURE_FAULT;
        for(rt_u32 i=0;i<3;i++) local.capture[capture_slots[i]].error=-5;
    }
    rt_u32 ticks=(rt_u32)(amp_ticks()-started); if(ticks>local.irq_max_ticks) local.irq_max_ticks=ticks;
}
static void analog_poll(rt_u64 now)
{
    rt_u32 i=(local.gpadc_updates[0]+local.gpadc_updates[1])&1U;
    /* 两组轮换；没有新pending不得刷新数据年龄。 */
    for(rt_u32 pass=0;pass<2;pass++,i^=1U) {
        rt_u32 base=i?GPADC2:GPADC0,mask=i?15U:0x70U,pending=rd(base+0x38U)&mask;
        if(!pending) continue;
        for(rt_u32 ch=0;ch<(i?4U:7U);ch++) if(pending&(1U<<ch)) {
            rt_u32 slot=ch, value=rd(base+0x80U+ch*4U);
            if(value&~4095U) local.gpadc_valid&=~(1U<<slot);
            else {
                local.gpadc_raw[slot]=value;
                local.gpadc_valid|=1U<<slot;
                local.gpadc_ticks[slot]=rt_words_make(now);
            }
        }
        wr(base+0x38U,pending); local.gpadc_updates[i]++; break;
    }
}
static void rtc_complete(rt_u64 now)
{
    if(bus.result) {
        local.rtc_bus_errors++;
        if(rtc_stage==3U) local.temperature_error=bus.result;
        else { local.rtc_error=bus.result; local.flags&=~AMP_RTIO_RTC_VALID; }
        if(rtc_stage>=10U) { local.rtc_request_id=request.request_id; local.rtc_request_result=bus.result; }
        rtc_stage=0; next_rtc=now+24000000U; return;
    }
    switch(rtc_stage) {
    case 1:
        memcpy(rtc_raw,bus.data,16);
        if(!rtc_ctrl_changed && (rtc_raw[14]&7U)!=4U) {
            rt_u8 ctrl=(rt_u8)((rtc_raw[14]&~7U)|4U);
            rtc_ctrl_saved=rtc_raw[14]; rtc_ctrl_changed=1;
            rtc_stage=4; i2c_begin(0x32,0xe0,1,&ctrl,now);
        } else { rtc_stage=2; i2c_begin(0x32,0,1,0,now); }
        break;
    case 4: rtc_raw[14]=(rt_u8)((rtc_raw[14]&~7U)|4U); rtc_stage=2; i2c_begin(0x32,0,1,0,now); break;
    case 2: {
        rt_u64 utc=0;
        /* 低速分步读可能跨秒；整块和重读秒不一致时拒绝该窗口，下一轮重新读。 */
        local.rtc_reads++; local.rtc_error=bus.data[0]!=rtc_raw[0]?-11:amp_rtio_decode_rtc(rtc_raw,&utc);
        local.rtc_ctrl=(rt_u32)rtc_raw[14]|((rt_u32)rtc_raw[15]<<8U);
        if(!local.rtc_error) { local.rtc_utc=rt_words_make(utc); local.rtc_ticks=rt_words_make(now); local.flags|=AMP_RTIO_RTC_VALID; }
        else local.flags&=~AMP_RTIO_RTC_VALID;
        rtc_stage=3; i2c_begin(0x48,0,2,0,now); break;
    }
    case 3: {
        rt_s32 raw=(rt_s16)(((rt_u32)bus.data[0]<<8U)|bus.data[1]);
        local.temperature_milli_c=(raw*1000)/256; local.temperature_error=0; local.temperature_ticks=rt_words_make(now);
        rtc_stage=0; next_rtc=now+24000000U; break;
    }
    case 10: {
        /* 写入只由显式Linux请求触发，24小时制与原Linux驱动一致。 */
        memcpy(rtc_raw,bus.data,16);
        rt_u8 ctrl=(rt_u8)(rtc_raw[14]|0x20U); rtc_raw[14]=ctrl; rtc_stage=11; i2c_begin(0x32,0xe0,1,&ctrl,now); break;
    }
    case 11: rtc_stage=12; i2c_begin(0x32,0,7,write_date,now); break;
    case 12: { rt_u8 ctrl=(rt_u8)((rtc_raw[15]&~0x50U)|0x20U); rtc_stage=13; i2c_begin(0x32,0xf0,1,&ctrl,now); break; }
    case 13: rtc_stage=14; i2c_begin(0x32,0,16,0,now); break;
    case 14: {
        rt_u64 actual=0,expected=0;
        rt_u8 written[16]={0};
        memcpy(written,write_date,7);written[14]=written[15]=0x20U;
        int result=amp_rtio_decode_rtc(bus.data,&actual);
        if(!result) result=amp_rtio_decode_rtc(written,&expected);
        if(!result && (actual<expected || actual-expected>1U)) result=-5;
        local.rtc_request_id=request.request_id; local.rtc_request_result=result;
        rtc_stage=0; next_rtc=now; break;
    }
    default:rtc_stage=0; break;
    }
}
static int command_read(struct amp_rtio_command *c)
{
#ifdef AMP_RTIO_HOST_TEST
    return amp_rtio_test_command(c);
#else
    const volatile struct amp_rtio_command *p=(const volatile struct amp_rtio_command *)(uintptr_t)(STATUS+AMP_RTIO_COMMAND_OFFSET);
    rt_u32 id=p->request_id;
    if(!id || id==last_request) return 0;
    memcpy(c,(const void *)p,sizeof(*c)); __asm__ volatile("dsb sy" ::: "memory");
    return id==p->request_id;
#endif
}
void amp_rtio_service(rt_u64 now)
{
    rt_u64 started=amp_ticks();
    if(!local.active) return;
    if(bus.state) { if(i2c_step(now)) rtc_complete(now); }
    else if(now>=next_analog) { analog_poll(now); next_analog=now+24000U; }
    else if(!rtc_stage && command_read(&request) && request.request_id!=last_request) {
        last_request=request.request_id;
        int error=request.version!=AMP_RTIO_VERSION || request.bytes!=sizeof(request) || request.epoch!=local.epoch ||
            request.operation!=AMP_RTIO_RTC_SET || request.crc32!=rt_crc32(&request,offsetof(struct amp_rtio_command,crc32))?-71:amp_rtio_encode_rtc(&request,write_date);
        if(error) { local.rtc_request_id=request.request_id; local.rtc_request_result=error; }
        else { rtc_stage=10; i2c_begin(0x32,0,16,0,now); }
    }
    else if(!rtc_stage && now>=next_rtc) { rtc_stage=1; i2c_begin(0x32,0,16,0,now); }
    rt_u32 ticks=(rt_u32)(amp_ticks()-started); if(ticks>local.service_max_ticks) local.service_max_ticks=ticks;
}
void amp_rtio_publish(rt_u32 run)
{
    struct amp_rtio_snapshot copy;
    rt_u64 started=amp_ticks();
    rt_u32 flags=irq_save(); local.run_generation=run; local.ticks=rt_words_make(amp_ticks());
    memcpy(&copy,&local,sizeof(copy)); irq_restore(flags);
    /* PPS只有秒沿没有绝对年月日；即使合格也不设置GPS UTC锁定位。 */
    if(!copy.capture[0].error && rt_words_value(copy.ticks)-rt_words_value(copy.capture[0].last_ticks)<48000000U) copy.flags|=AMP_RTIO_GPS_PERIOD_VALID;
#ifdef AMP_RTIO_HOST_TEST
    amp_rtio_test_snapshot(&copy);
#else
    volatile struct amp_rtio_snapshot *out=(volatile struct amp_rtio_snapshot *)(uintptr_t)(STATUS+AMP_RTIO_OFFSET);
    rt_u32 seq=out->seq_lock+1U; out->seq_lock=seq|1U; __asm__ volatile("dsb sy" ::: "memory");
    copy.seq_lock=seq|1U;
    /* Device共享页用整字复制，不能调用逐字节memcpy扩大采样盲窗。 */
    for(rt_u32 i=0;i<sizeof(copy)/4U;i++) ((volatile rt_u32 *)out)[i]=((const rt_u32 *)&copy)[i];
    __asm__ volatile("dsb sy" ::: "memory");
    out->seq_lock=(seq|1U)+1U; __asm__ volatile("dsb sy" ::: "memory");
#endif
    rt_u32 elapsed=(rt_u32)(amp_ticks()-started);
    if(elapsed>local.publish_max_ticks) local.publish_max_ticks=elapsed;
}
#endif
