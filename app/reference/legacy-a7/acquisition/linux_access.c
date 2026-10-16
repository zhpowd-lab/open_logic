// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include "access.h"
#include "protocol.h"
#include "linux_transport.h"
#include "linux_realtime_io.h"

/* 本模块仅适用于隔离实验DT；默认不会claim，不触碰ADC总线和转换。 */
static struct device_node *node;
static void __iomem *status_page;
static struct clk *clocks[4], *parent;
static struct reset_control *lbc_reset;
static struct pwm_device *pwm10, *pwm11;
static struct resource *lbc_region;
static int gpios[25], gpio_count, clocks_enabled, held, pinned;
static int stop_attempted, stop_acknowledged;
static atomic_t opened = ATOMIC_INIT(0);
static DEFINE_MUTEX(access_lock);
static struct miscdevice access_device;

static int resources_active(void)
{
    return READ_ONCE(held) && atomic_read(&opened);
}

static int snapshot(struct amp_acq_status *out)
{
    unsigned retry;
    for (retry = 0; retry < 1000; retry++) {
        u32 first = readl(status_page + offsetof(struct amp_acq_status, seq_lock));
        if (first & 1U)
            continue;
        memcpy_fromio(out, status_page, sizeof(*out));
        rmb();
        if (first == readl(status_page + offsetof(struct amp_acq_status, seq_lock)) &&
            first == out->seq_lock)
            return out->magic == AMP_ACQ_MAGIC && out->version == AMP_ACQ_VERSION &&
                out->bytes == sizeof(*out) && out->cpu == 3 ? 0 : -EPROTO;
    }
    return -EAGAIN;
}

static int e907_offline(void)
{
    struct device_node *remote_node = of_parse_phandle(node, "e907", 0);
    struct rproc *remote;
    int result;
    if (!remote_node)
        return -ENODEV;
    remote = rproc_get_by_phandle(remote_node->phandle);
    of_node_put(remote_node);
    if (!remote)
        return -ENODEV;
    mutex_lock(&remote->lock);
    result = remote->state == RPROC_OFFLINE ? 0 : -EBUSY;
    mutex_unlock(&remote->lock);
    rproc_put(remote);
    return result;
}

static int running_child(struct device *device, void *unused)
{
    struct rproc *remote;
    int result;
    if (!device->class || strcmp(device->class->name, "remoteproc"))
        return 0;
    remote = container_of(device, struct rproc, dev);
    mutex_lock(&remote->lock);
    result = remote->state == RPROC_RUNNING;
    mutex_unlock(&remote->lock);
    return result;
}

static int a7_running(void)
{
    struct device_node *remote_node, *amp;
    struct platform_device *platform;
    u32 cpu;
    int result = -ENODEV;
    remote_node = of_find_node_by_path("/soc@3000000/rt_mpu_a7_rproc@0");
    if (!remote_node)
        return result;
    amp = of_parse_phandle(remote_node, "boot-amp", 0);
    if (!amp || !of_device_is_compatible(remote_node, "allwinner,arm-barematal-rproc") ||
        of_property_count_u32_elems(amp, "cpu-id") != 1 ||
        of_property_read_u32(amp, "cpu-id", &cpu) || cpu != 3)
        goto out;
    platform = of_find_device_by_node(remote_node);
    if (!platform)
        goto out;
    result = device_for_each_child(&platform->dev, NULL, running_child) == 1 ? 0 : -EBUSY;
    put_device(&platform->dev);
out:
    of_node_put(amp);
    of_node_put(remote_node);
    return result;
}

static void drop_resources(void)
{
    while (clocks_enabled)
        clk_disable_unprepare(clocks[--clocks_enabled]);
    while (gpio_count)
        gpio_free(gpios[--gpio_count]);
    if (pwm11) { pwm_put(pwm11); pwm11 = NULL; }
    if (pwm10) { pwm_put(pwm10); pwm10 = NULL; }
    if (lbc_region) { release_mem_region(0x02810000, 0x1000); lbc_region = NULL; }
    held = 0;
    if (pinned) {
        pinned = 0;
        module_put(THIS_MODULE);
    }
}

static int claim_resources(void)
{
    struct amp_acq_status state, after;
    int i, result;
    if (held)
        return -EBUSY;
    if (snapshot(&state) || state.state != RT_STATE_STOPPED || !state.stopped ||
        state.run_generation || cpu_online(3))
        return -EBUSY;
    if (a7_running())
        return -EBUSY;
    usleep_range(2000, 3000);
    if (snapshot(&after) || after.epoch != state.epoch || after.state != state.state ||
        !after.stopped || rt_words_value(after.ticks) <= rt_words_value(state.ticks))
        return -ESTALE;
    result = e907_offline();
    if (result)
        return result;
    /* Linux原生占用防止其它Linux驱动抢资源；实验控制端仍须禁止E907再次start。 */
    lbc_region = request_mem_region(0x02810000, 0x1000, "rt_mpu_a7_ida");
    if (!lbc_region)
        return -EBUSY;
    pwm10 = of_pwm_get(access_device.this_device, node, "convst");
    if (IS_ERR(pwm10)) { result = PTR_ERR(pwm10); pwm10 = NULL; goto fail; }
    pwm11 = of_pwm_get(access_device.this_device, node, "clock-peer");
    if (IS_ERR(pwm11)) { result = PTR_ERR(pwm11); pwm11 = NULL; goto fail; }
    if (pwm_is_enabled(pwm10) || pwm_is_enabled(pwm11)) { result = -EBUSY; goto fail; }
    for (i = 0; i < ARRAY_SIZE(gpios); i++) {
        result = of_get_named_gpio(node, "adc-gpios", i);
        if (result < 0)
            goto fail;
        gpios[i] = result;
        result = gpio_request(gpios[i], "rt_mpu_a7_ida");
        if (result)
            goto fail;
        gpio_count++;
    }
    if (clk_get_rate(parent) != 400000000) { result = -EINVAL; goto fail; }
    result = clk_set_parent(clocks[2], parent);
    if (!result)
        result = clk_set_rate(clocks[2], 100000000);
    if (result || clk_get_rate(clocks[2]) != 100000000) {
        result = result ? result : -EINVAL;
        goto fail;
    }
    for (i = 0; i < ARRAY_SIZE(clocks); i++) {
        result = clk_prepare_enable(clocks[i]);
        if (result)
            goto fail;
        clocks_enabled++;
    }
    result = reset_control_assert(lbc_reset);
    if (!result)
        result = reset_control_deassert(lbc_reset);
    /* PWM2的reset由现有PWM提供者持有，禁止为ADC复位整个共享PWM控制器。 */
    if (result)
        goto fail;
    if (!try_module_get(THIS_MODULE)) { result = -ENODEV; goto fail; }
    result = amp_rtio_linux_claim();
    if (result) { module_put(THIS_MODULE); goto fail; }
    pinned = held = 1;
    stop_attempted = stop_acknowledged = 0;
    return 0;
fail:
    drop_resources();
    return result;
}

static int release_resources(void)
{
    struct amp_acq_status state;
    int result;
    if (!held)
        return 0;
    /* 不能用旧STOPPED快照释放：队列中可能有尚未处理的START。先匹配本次STOP的ACK。 */
    if (!stop_attempted) {
        stop_attempted = 1;
        result = amp_transport_stop_remote();
        stop_acknowledged = result == 0;
        if (result)
            return result;
    }
    if (!stop_acknowledged)
        return -EBUSY;
    if (snapshot(&state) || !state.stopped ||
        (state.state != RT_STATE_STOPPED && state.state != RT_STATE_FAULT))
        return -EBUSY;
    result = amp_rtio_linux_release();
    if (result) return result;
    drop_resources();
    return 0;
}

static int access_open(struct inode *inode, struct file *file)
{
    if (!capable(CAP_SYS_RAWIO) || !(file->f_mode & FMODE_WRITE))
        return -EPERM;
    return atomic_cmpxchg(&opened, 0, 1) ? -EBUSY : 0;
}

static int access_release(struct inode *inode, struct file *file)
{
    int result = 0;
    mutex_lock(&access_lock);
    /* 最后一个映射/控制fd退出后已无用户命令生产者；只发一次STOP，失败保留资源。 */
    if (held)
        result = release_resources();
    if (result)
        pr_err("RT_MPU A7 resources retained: ADC stop not confirmed\n");
    atomic_set(&opened, 0);
    mutex_unlock(&access_lock);
    return 0;
}

static long access_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
    int result;
    if (argument)
        return -EINVAL;
    mutex_lock(&access_lock);
    if (command == AMP_TEST_CLAIM)
        result = claim_resources();
    else if (command == AMP_TEST_RELEASE)
        result = release_resources();
    else
        result = -ENOTTY;
    mutex_unlock(&access_lock);
    return result;
}

static int access_mmap(struct file *file, struct vm_area_struct *area)
{
    unsigned long bytes = area->vm_end - area->vm_start;
    int result = -EPERM;
    mutex_lock(&access_lock);
    if (area->vm_flags & VM_EXEC)
        goto out;
    if (area->vm_pgoff == (AMP_ACQ_STATUS_ADDRESS >> PAGE_SHIFT) && bytes == PAGE_SIZE && !(area->vm_flags & VM_WRITE))
        area->vm_flags &= ~VM_MAYWRITE;
    else if (area->vm_pgoff != (AMP_ACQ_RING_ADDRESS >> PAGE_SHIFT) || bytes != RT_REGION_BYTES || !held)
        goto out;
    area->vm_flags &= ~VM_MAYEXEC;
    area->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
    area->vm_page_prot = area->vm_pgoff == (AMP_ACQ_RING_ADDRESS >> PAGE_SHIFT) ?
        pgprot_writecombine(area->vm_page_prot) : pgprot_noncached(area->vm_page_prot);
    result = remap_pfn_range(area, area->vm_start, area->vm_pgoff, bytes, area->vm_page_prot);
out:
    mutex_unlock(&access_lock);
    return result;
}

static const struct file_operations access_ops = {
    .owner = THIS_MODULE, .open = access_open, .release = access_release,
    .unlocked_ioctl = access_ioctl, .mmap = access_mmap, .llseek = no_llseek,
};
static struct miscdevice access_device = {
    .minor = MISC_DYNAMIC_MINOR, .name = "rt_mpu_amp_acq", .mode = 0600, .fops = &access_ops,
};

static void put_handles(void)
{
    int i;
    for (i = 0; i < ARRAY_SIZE(clocks); i++)
        if (!IS_ERR_OR_NULL(clocks[i])) clk_put(clocks[i]);
    if (!IS_ERR_OR_NULL(parent)) clk_put(parent);
    if (!IS_ERR_OR_NULL(lbc_reset)) reset_control_put(lbc_reset);
    if (status_page) iounmap(status_page);
    of_node_put(node);
}

static int __init access_init(void)
{
    static const char * const names[] = {"mbus", "bus", "lbc", "pwm"};
    struct device_node *memory;
    struct resource region;
    int i, result = -ENODEV;
    node = of_find_compatible_node(NULL, NULL, "rtmpu,a7-acquisition-test-v1");
    memory = of_find_node_by_path("/reserved-memory/rt_mpu_a7@44200000");
    if (!node || !memory)
        goto invalid;
    result = of_address_to_resource(memory, 0, &region);
    if (result || !of_property_read_bool(memory, "no-map") ||
        region.start != 0x44200000 || resource_size(&region) != 0x200000 ||
        of_gpio_named_count(node, "adc-gpios") != ARRAY_SIZE(gpios)) {
        result = -EINVAL;
        goto invalid;
    }
    status_page = ioremap(AMP_ACQ_STATUS_ADDRESS, PAGE_SIZE);
    if (!status_page) { result = -ENOMEM; goto invalid; }
    for (i = 0; i < ARRAY_SIZE(clocks); i++) {
        clocks[i] = of_clk_get_by_name(node, names[i]);
        if (IS_ERR(clocks[i])) { result = PTR_ERR(clocks[i]); goto invalid; }
    }
    parent = of_clk_get_by_name(node, "parent");
    if (IS_ERR(parent)) { result = PTR_ERR(parent); goto invalid; }
    lbc_reset = of_reset_control_get_exclusive(node, "lbc");
    if (IS_ERR(lbc_reset)) { result = PTR_ERR(lbc_reset); goto invalid; }
    result = misc_register(&access_device);
    if (!result) {
        result = amp_rtio_linux_init(status_page);
        if (!result) {
            result = amp_transport_start(snapshot, resources_active);
            if (result) amp_rtio_linux_exit();
        }
        if (!result) {
            of_node_put(memory);
            return 0;
        }
        misc_deregister(&access_device);
    }
invalid:
    of_node_put(memory);
    put_handles();
    return result;
}

static void __exit access_exit(void)
{
    misc_deregister(&access_device);
    amp_transport_stop();
    amp_rtio_linux_exit();
    put_handles();
}
module_init(access_init);
module_exit(access_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RT_MPU isolated A7 IDA experiment resources and reserved RAM access");
