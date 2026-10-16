// SPDX-License-Identifier: GPL-2.0
#include <linux/capability.h>
#include <linux/clk.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <asm/arch_timer.h>
#include "linux_realtime_io.h"
#include "realtime_io_ioctl.h"
#include "rt_shared_ring.h"

#if AMP_REALTIME_IO
static struct device_node *rtio_node;
static void __iomem *rtio_page, *pwmcs;
static struct clk *rtio_clocks[10];
static struct reset_control *rtio_resets[4];
static struct pwm_device *capture_pwms[3];
static struct resource *regions[4];
static int rtio_gpios[12], pin_count, clock_count, reset_count, pwm_count;
static unsigned int capture_irq;
static int irq_disabled, owned;
static u32 last_submitted, request_serial;
static DEFINE_MUTEX(rtio_lock);
static const char * const clock_names[] = {
    "twi", "apb", "pwmcs", "pwmcs-bus", "gpadc0", "gpadc0-bus",
    "gpadc2", "gpadc2-bus", "rtp", "rtp-bus"
};
static const char * const reset_names[] = {"twi", "gpadc0", "gpadc2", "rtp"};
static const unsigned long region_addresses[] = {0x02511000,0x02087000,0x02089000,0x0208c000};
static const char * const disabled_nodes[] = {
    "/soc@3000000/twi1@2511000", "/soc@3000000/gpadc@2087000",
    "/soc@3000000/gpadc@2089000", "/soc@3000000/rtp@208c000"
};

static int rtio_snapshot(struct amp_rtio_snapshot *out)
{
    unsigned int i;
    void __iomem *source = rtio_page + AMP_RTIO_OFFSET;
    for (i=0;i<20;i++) {
        u32 seq=readl(source+offsetof(struct amp_rtio_snapshot,seq_lock));
        if(seq&1U) continue;
        memcpy_fromio(out,source,sizeof(*out));
        rmb();
        if(seq==out->seq_lock && seq==readl(source+offsetof(struct amp_rtio_snapshot,seq_lock)))
            return out->magic==AMP_RTIO_MAGIC && out->version==AMP_RTIO_VERSION &&
                out->bytes==sizeof(*out) ? 0 : -EPROTO;
    }
    return -EAGAIN;
}
static long aux_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
    struct amp_rtio_snapshot state;
    struct amp_rtio_command request;
    unsigned int i;
    int result;
    void __iomem *target=rtio_page+AMP_RTIO_COMMAND_OFFSET;
    mutex_lock(&rtio_lock);
    result=rtio_snapshot(&state);
    if(result) goto out;
    if(command==AMP_AUX_SNAPSHOT) {
        state.observed_ticks=rt_words_make(__arch_counter_get_cntpct_stable());
        state.observed_monotonic_ns=rt_words_make(ktime_get_ns());
        result=copy_to_user((void __user *)argument,&state,sizeof(state)) ? -EFAULT : 0;
        goto out;
    }
    if(command!=AMP_AUX_RTC_SET) { result=-ENOTTY; goto out; }
    if(!capable(CAP_SYS_TIME) || !(file->f_mode&FMODE_WRITE)) { result=-EPERM; goto out; }
    if(!owned || !state.active || !state.run_generation) { result=-ENODEV; goto out; }
    if(copy_from_user(&request,(void __user *)argument,sizeof(request))) { result=-EFAULT; goto out; }
    if(request.epoch!=state.epoch || request.request_id || request.crc32 ||
       request.version!=AMP_RTIO_VERSION || request.bytes!=sizeof(request) || request.operation!=AMP_RTIO_RTC_SET) {
        result=-EINVAL; goto out;
    }
    if(last_submitted && state.rtc_request_id!=last_submitted) { result=-EBUSY; goto out; }
    {
        rt_u8 date[7];
        result=amp_rtio_encode_rtc(&request,date);
        if(result) goto out;
    }
    if(++request_serial==0) ++request_serial;
    request.request_id=request_serial;
    request.crc32=rt_crc32(&request,offsetof(struct amp_rtio_command,crc32));
    /* request_id最后发布；即使用户拷贝结果失败，已提交的写也不能自动重发。 */
    writel(0,target+offsetof(struct amp_rtio_command,request_id));
    wmb();
    for(i=0;i<sizeof(request)/4;i++)
        if(i!=offsetof(struct amp_rtio_command,request_id)/4)
            writel(((u32 *)&request)[i],target+i*4);
    wmb(); writel(request.request_id,target+offsetof(struct amp_rtio_command,request_id));
    last_submitted=request.request_id;
    result=copy_to_user((void __user *)argument,&request,sizeof(request)) ? -EFAULT : 0;
out:
    mutex_unlock(&rtio_lock);
    return result;
}
static const struct file_operations aux_ops={
    .owner=THIS_MODULE,.unlocked_ioctl=aux_ioctl,.llseek=no_llseek,
};
static struct miscdevice aux_device={
    .minor=MISC_DYNAMIC_MINOR,.name="rt_mpu_aux",.mode=0600,.fops=&aux_ops,
};
static void drop_rtio(void)
{
    int i;
    if(irq_disabled) { enable_irq(capture_irq); irq_disabled=0; }
    while(reset_count) reset_control_assert(rtio_resets[--reset_count]);
    while(clock_count) clk_disable_unprepare(rtio_clocks[--clock_count]);
    while(pin_count) gpio_free(rtio_gpios[--pin_count]);
    while(pwm_count) { pwm_put(capture_pwms[--pwm_count]); capture_pwms[pwm_count]=NULL; }
    for(i=3;i>=0;i--) if(regions[i]) {
        release_mem_region(region_addresses[i],0x400); regions[i]=NULL;
    }
    owned=0;
}
int amp_rtio_linux_claim(void)
{
    static const char * const names[]={"gps","rtc","frequency1"};
    struct amp_rtio_snapshot state;
    unsigned int i;
    int result;
    mutex_lock(&rtio_lock);
    if(owned) { result=-EBUSY; goto out; }
    result=rtio_snapshot(&state);
    if(result || state.active) { result=result ? result : -EBUSY; goto out; }
    for(i=0;i<4;i++) {
        struct device_node *n=of_find_node_by_path(disabled_nodes[i]);
        bool disabled=n && !of_device_is_available(n);
        of_node_put(n);
        if(!disabled) { result=-EBUSY; goto fail; }
        regions[i]=request_mem_region(region_addresses[i],0x400,"rt_mpu_a7_rtio");
        if(!regions[i]) { result=-EBUSY; goto fail; }
    }
    for(i=0;i<3;i++) {
        capture_pwms[i]=of_pwm_get(aux_device.this_device,rtio_node,names[i]);
        if(IS_ERR(capture_pwms[i])) { result=PTR_ERR(capture_pwms[i]); capture_pwms[i]=NULL; goto fail; }
        pwm_count++;
        if(pwm_is_enabled(capture_pwms[i])) { result=-EBUSY; goto fail; }
    }
    for(i=0;i<12;i++) {
        result=of_get_named_gpio(rtio_node,"rtio-gpios",i);
        if(result<0) goto fail;
        rtio_gpios[i]=result;
        result=gpio_request(result,"rt_mpu_a7_rtio");
        if(result) goto fail;
        pin_count++;
    }
    if(clk_get_rate(rtio_clocks[1])!=24000000 || clk_get_rate(rtio_clocks[4])!=24000000 ||
       clk_get_rate(rtio_clocks[6])!=24000000) { result=-EINVAL; goto fail; }
    for(i=0;i<ARRAY_SIZE(rtio_clocks);i++) {
        result=clk_prepare_enable(rtio_clocks[i]); if(result) goto fail; clock_count++;
    }
    for(i=0;i<4;i++) {
        result=reset_control_assert(rtio_resets[i]); if(result) goto fail;
        result=reset_control_deassert(rtio_resets[i]); if(result) goto fail; reset_count++;
    }
    /* PWMCS0是CPU供电提供者，绝不复位/解绑或改parent。只隔离它的空闲捕获IRQ。 */
    for(i=1;i<=4;i++) {
        void __iomem *g=pwmcs+i*0x1000;
        if(readl(g) || readl(g+8) || readl(g+0x10) || readl(g+0x18)) { result=-EBUSY; goto fail; }
    }
    disable_irq(capture_irq); irq_disabled=1;
    owned=1; last_submitted=0;
    result=0; goto out;
fail:
    drop_rtio();
out:
    mutex_unlock(&rtio_lock); return result;
}
int amp_rtio_linux_release(void)
{
    struct amp_rtio_snapshot state;
    int result=0;
    mutex_lock(&rtio_lock);
    if(owned) {
        result=rtio_snapshot(&state);
        if(!result && state.active) result=-EBUSY;
        if(!result) drop_rtio();
    }
    mutex_unlock(&rtio_lock); return result;
}
void amp_rtio_linux_exit(void)
{
    unsigned int i;
    misc_deregister(&aux_device);
    for(i=0;i<10;i++) if(!IS_ERR_OR_NULL(rtio_clocks[i])) clk_put(rtio_clocks[i]);
    for(i=0;i<4;i++) if(!IS_ERR_OR_NULL(rtio_resets[i])) reset_control_put(rtio_resets[i]);
    if(pwmcs) iounmap(pwmcs);
    of_node_put(rtio_node); rtio_node=NULL;
}
static int capture_irq_matches(struct irq_data *data)
{
    while(data && data->parent_data) data=data->parent_data;
    return data && data->hwirq==AMP_RTIO_IRQ;
}
int amp_rtio_linux_init(void __iomem *page)
{
    struct device_node *provider;
    struct irq_data *capture_data;
    unsigned int i;
    int result=-ENODEV;
    rtio_page=page;
    rtio_node=of_find_compatible_node(NULL,NULL,"rtmpu,a7-realtime-io-v1");
    if(!rtio_node || of_gpio_named_count(rtio_node,"rtio-gpios")!=12) goto fail;
    for(i=0;i<10;i++) {
        rtio_clocks[i]=of_clk_get_by_name(rtio_node,clock_names[i]);
        if(IS_ERR(rtio_clocks[i])) { result=PTR_ERR(rtio_clocks[i]); goto fail; }
    }
    for(i=0;i<4;i++) {
        rtio_resets[i]=of_reset_control_get_exclusive(rtio_node,reset_names[i]);
        if(IS_ERR(rtio_resets[i])) { result=PTR_ERR(rtio_resets[i]); goto fail; }
    }
    provider=of_parse_phandle(rtio_node,"capture-controller",0);
    if(!provider) goto fail;
    result=of_irq_get(provider,0);
    of_node_put(provider);
    if(result<0) goto fail;
    capture_irq=result;
    /* T153先经过wakeupgen域（SPI19），裸A7使用最终GIC编号51，不能混用两级hwirq。 */
    capture_data=irq_get_irq_data(capture_irq);
    if(!capture_irq_matches(capture_data)) { result=-EINVAL; goto fail; }
    pwmcs=ioremap(0x020a0000,0x5000);
    if(!pwmcs) { result=-ENOMEM; goto fail; }
    result=misc_register(&aux_device);
    if(!result) return 0;
fail:
    /* 注册前失败不能deregister未注册的设备。 */
    for(i=0;i<10;i++) if(!IS_ERR_OR_NULL(rtio_clocks[i])) clk_put(rtio_clocks[i]);
    for(i=0;i<4;i++) if(!IS_ERR_OR_NULL(rtio_resets[i])) reset_control_put(rtio_resets[i]);
    if(pwmcs) iounmap(pwmcs);
    of_node_put(rtio_node); rtio_node=NULL;
    return result;
}
#endif
