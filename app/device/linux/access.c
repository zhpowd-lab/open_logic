// SPDX-License-Identifier: GPL-2.0
#include <linux/capability.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/uaccess.h>
#include "../logic16.h"
#include "../firmware/board-registers.h"

#define L16_SNAPSHOT_RETRIES 100U /* 状态 seqlock 的有界重试次数。 */
#define L16_MMAP_PAGE_OFFSET 1U /* mmap 仅公开原码，状态通过 read 获取。 */
#define L16_DEVICE_MODE 0600 /* 仅管理员可申请总线资源。 */

static struct device_node *node;
static void __iomem *status;
#define L16_CLOCK_COUNT 3U /* mbus、bus、lbc 三个时钟句柄。 */
static struct clk *clocks[L16_CLOCK_COUNT], *parent, *previous_parent;
static unsigned long previous_rate;
static struct reset_control *reset;
static struct resource *region;
static int gpios[L16_GPIO_COUNT], gpio_count, clock_count;
static bool held, opened, clock_changed;
static DEFINE_MUTEX(lock);

static int snapshot(struct l16_status *out)
{
    unsigned int retry;
    for (retry = 0; retry < L16_SNAPSHOT_RETRIES; retry++) {
        u32 seq = readl(status);
        if (seq & 1) continue;
        memcpy_fromio(out, status, sizeof(*out));
        rmb();
        if (seq == readl(status) && seq == out->seq)
            return out->magic == L16_MAGIC && out->version == L16_VERSION &&
                out->bytes == sizeof(*out) && out->cpu == L16_CPU &&
                out->count <= L16_MAX_SAMPLES ? 0 : -EPROTO;
    }
    return -EAGAIN;
}

static int e907_idle(void)
{
    struct device_node *e907 = of_parse_phandle(node, "e907", 0);
    struct rproc *remote;
    int result = 0;
    if (!e907) return -ENODEV;
    remote = rproc_get_by_phandle(e907->phandle);
    of_node_put(e907);
    if (!remote) return -ENODEV;
    mutex_lock(&remote->lock);
    result = remote->state == RPROC_OFFLINE ? 0 : -EBUSY;
    mutex_unlock(&remote->lock);
    rproc_put(remote);
    return result;
}

static int drop_resources(void)
{
    int result = 0;
    while (clock_count) clk_disable_unprepare(clocks[--clock_count]);
    if (clock_changed) {
        result = clk_set_parent(clocks[L16_CLOCK_COUNT - 1], previous_parent);
        if (!result) result = clk_set_rate(clocks[L16_CLOCK_COUNT - 1], previous_rate);
        clock_changed = false;
    }
    while (gpio_count) gpio_free(gpios[--gpio_count]);
    if (region) {
        release_mem_region(L16_LBC_BASE, L16_LBC_MAP_BYTES);
        region = NULL;
    }
    return result;
}

static int claim(void)
{
    struct l16_status state;
    int result, i;
    if (held) return 0;
    if (cpu_online(L16_CPU) || snapshot(&state) || state.state != L16_IDLE || state.capture_id || e907_idle()) return -EBUSY;
    region = request_mem_region(L16_LBC_BASE, L16_LBC_MAP_BYTES, "open_logic16");
    if (!region) return -EBUSY;
    /* 只持有总线的 22 个引脚，不申请 ADC RESET/BUSY、PWM 或开出。 */
    for (i = 0; i < ARRAY_SIZE(gpios); i++) {
        result = of_get_named_gpio(node, "adc-gpios", i + L16_DT_GPIO_FIRST);
        if (result < 0) goto fail;
        gpios[i] = result;
        result = gpio_request(gpios[i], "open_logic16");
        if (result) goto fail;
        gpio_count++;
    }
    if (clk_get_rate(parent) != L16_LBC_PARENT_HZ) {
        result = -EINVAL;
        goto fail;
    }
    previous_parent = clk_get_parent(clocks[L16_CLOCK_COUNT - 1]);
    previous_rate = clk_get_rate(clocks[L16_CLOCK_COUNT - 1]);
    clock_changed = true;
    result = clk_set_parent(clocks[L16_CLOCK_COUNT - 1], parent);
    if (!result) result = clk_set_rate(clocks[L16_CLOCK_COUNT - 1], L16_LBC_CLOCK_HZ);
    if (result || clk_get_rate(clocks[L16_CLOCK_COUNT - 1]) != L16_LBC_CLOCK_HZ) {
        result = result ? result : -EINVAL;
        goto fail;
    }
    for (i = 0; i < ARRAY_SIZE(clocks); i++) {
        result = clk_prepare_enable(clocks[i]);
        if (result) goto fail;
        clock_count++;
    }
    result = reset_control_assert(reset);
    if (!result) result = reset_control_deassert(reset);
    if (result) goto fail;
    if (!try_module_get(THIS_MODULE)) {
        result = -ENODEV;
        goto fail;
    }
    held = true;
    return 0;
fail:
    if (drop_resources()) pr_err("logic16: clock restoration failed after claim failure\n");
    return result;
}

static int release_resources(void)
{
    struct l16_status state;
    int result;
    if (!held) return 0;
    /* 采集停止不等于裸核关闭；仅在 SGI/PSCI 关闭确认后归还时钟和引脚。 */
    if (snapshot(&state) || state.state != L16_OFF || !cpu_online(L16_CPU)) return -EBUSY;
    result = drop_resources();
    held = false;
    module_put(THIS_MODULE);
    return result;
}

static int open_device(struct inode *inode, struct file *file)
{
    int result = 0;
    if (!capable(CAP_SYS_RAWIO)) return -EPERM;
    mutex_lock(&lock);
    if (opened) result = -EBUSY;
    else opened = true;
    mutex_unlock(&lock);
    return result;
}

static int close_device(struct inode *inode, struct file *file)
{
    mutex_lock(&lock);
    if (held && release_resources())
        pr_warn("logic16: release failed; resources retained=%u\n", held);
    opened = false;
    mutex_unlock(&lock);
    return 0;
}

static ssize_t read_device(struct file *file, char __user *output, size_t size, loff_t *offset)
{
    struct l16_status state;
    int result;
    if (size < sizeof(state)) return -EINVAL;
    result = snapshot(&state);
    if (result) return result;
    return copy_to_user(output, &state, sizeof(state)) ? -EFAULT : sizeof(state);
}

static long control_device(struct file *file, unsigned int command, unsigned long argument)
{
    struct l16_request request;
    struct l16_status state;
    int result = 0;
    mutex_lock(&lock);
    if (command == L16_CLAIM && !argument) result = claim();
    else if (command == L16_RELEASE && !argument) result = release_resources();
    else if (command == L16_START || (command == L16_ENABLE_MONITOR && !argument)) {
        if (!held || cpu_online(L16_CPU) || snapshot(&state) || state.state == L16_OFF || e907_idle()) {
            result = -EBUSY;
            goto out;
        }
        if (state.command_gen != state.command_ack) {
            result = -EAGAIN;
            goto out;
        }
        if (state.state == L16_RUNNING) {
            result = -EBUSY;
            goto out;
        }
        if (command == L16_ENABLE_MONITOR) {
            writel(L16_MONITOR, status + offsetof(struct l16_status, command));
            wmb();
            writel(state.command_gen + 1U, status + offsetof(struct l16_status, command_gen));
            goto out;
        }
        if (copy_from_user(&request, (void __user *)argument, sizeof(request))) {
            result = -EFAULT;
            goto out;
        }
        if (request.period_ticks < L16_MIN_PERIOD || request.period_ticks > L16_CLOCK ||
            !request.sample_limit || request.sample_limit > l16_capacity(request.period_ticks) ||
            request.pattern > 1 || request.reserved) {
            result = -EINVAL;
            goto out;
        }
        writel(request.period_ticks, status + offsetof(struct l16_status, request_period));
        writel(request.sample_limit, status + offsetof(struct l16_status, request_limit));
        writel(request.pattern, status + offsetof(struct l16_status, request_pattern));
        writel(L16_ARM, status + offsetof(struct l16_status, command));
        wmb();
        writel(state.command_gen + 1U, status + offsetof(struct l16_status, command_gen));
    } else result = -ENOTTY;
out:
    mutex_unlock(&lock);
    return result;
}

static int map_device(struct file *file, struct vm_area_struct *area)
{
    unsigned long bytes = area->vm_end - area->vm_start, address;
    if (area->vm_flags & (VM_WRITE | VM_EXEC)) return -EPERM;
    if (area->vm_pgoff != L16_MMAP_PAGE_OFFSET || bytes != PAGE_ALIGN(L16_SAMPLE_BYTES) || !held) return -EINVAL;
    address = L16_SAMPLE_ADDRESS;
    area->vm_flags &= ~(VM_MAYWRITE | VM_MAYEXEC);
    area->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
    area->vm_page_prot = pgprot_writecombine(area->vm_page_prot);
    return remap_pfn_range(area, area->vm_start, address >> PAGE_SHIFT, bytes, area->vm_page_prot);
}

static const struct file_operations operations = {
    .owner = THIS_MODULE, .open = open_device, .release = close_device, .read = read_device,
    .unlocked_ioctl = control_device, .mmap = map_device, .llseek = no_llseek,
};
static struct miscdevice device = {
    .minor = MISC_DYNAMIC_MINOR, .name = "logic16", .mode = L16_DEVICE_MODE, .fops = &operations
};

static void put_handles(void)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(clocks); i++) if (!IS_ERR_OR_NULL(clocks[i])) clk_put(clocks[i]);
    if (!IS_ERR_OR_NULL(parent)) clk_put(parent);
    if (!IS_ERR_OR_NULL(reset)) reset_control_put(reset);
    if (status) iounmap(status);
    of_node_put(node);
}

static int __init initialize(void)
{
    const char * const names[] = {
        "mbus", "bus", "lbc"
    };
    struct device_node *memory;
    struct resource reserved;
    unsigned int i;
    int result = -ENODEV;
    node = of_find_compatible_node(NULL, NULL, "rtmpu,a7-acquisition-test-v1");
    memory = of_find_node_by_path("/reserved-memory/rt_mpu_a7@44200000");
    if (!node || !memory) goto out;
    result = of_address_to_resource(memory, 0, &reserved);
    if (result || !of_property_read_bool(memory, "no-map") || reserved.start != L16_RESERVED_BASE ||
        resource_size(&reserved) != L16_RESERVED_BYTES || of_gpio_named_count(node, "adc-gpios") != L16_DT_GPIO_COUNT) {
        result = -EINVAL;
        goto out;
    }
    status = ioremap(L16_STATUS_ADDRESS, PAGE_SIZE);
    if (!status) {
        result = -ENOMEM;
        goto out;
    }
    for (i = 0; i < ARRAY_SIZE(clocks); i++) {
        clocks[i] = of_clk_get_by_name(node, names[i]);
        if (IS_ERR(clocks[i])) {
            result = PTR_ERR(clocks[i]);
            goto out;
        }
    }
    parent = of_clk_get_by_name(node, "parent");
    if (IS_ERR(parent)) {
        result = PTR_ERR(parent);
        goto out;
    }
    reset = of_reset_control_get_exclusive(node, "lbc");
    if (IS_ERR(reset)) {
        result = PTR_ERR(reset);
        goto out;
    }
    result = misc_register(&device);
out:
    of_node_put(memory);
    if (result) put_handles();
    return result;
}

static void __exit finalize(void) {
    misc_deregister(&device);
    put_handles();
}
module_init(initialize);
module_exit(finalize);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Independent CPU3 logic16 resources and bounded read-only capture access");
