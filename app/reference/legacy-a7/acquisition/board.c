#include "board.h"
#include "adc_convst.h"
#include "rt_acquisition_target.h"
#include "rates.h"

#if ADC_ACQ_CORE != ADC_CORE_A7_AMP || ADC_AMP_HEARTBEAT_ONLY || \
    (ADC_LBC_MODE != ADC_LBC_IDA_PIO && ADC_LBC_MODE != ADC_LBC_DA)
#error "This acquisition port implements A7 IDA+PIO or DA"
#endif
#if AMP_SELECT_SETTLE_TICKS < 12 || AMP_SELECT_SETTLE_TICKS > 240
#error "Select settling must remain within the reviewed experiment range"
#endif

/* 地址/字段来自T153 LBC v2、sun8iw22 CCU/GPIO及本板已核对的E907端口。 */
#define GPIO 0x03604000U
#define LBC 0x02810000U
#define CCU 0x02002000U
#define PWM RT_CONVST_PWM_BASE
#define DA_CS3 0xf0000000U
#ifndef AMP_IDA_FIXED_READ
#define AMP_IDA_FIXED_READ 0
#endif

#ifdef AMP_BOARD_HOST_TEST
/* 宿主模型通过寄存器公开行为验证端口，生产构建没有测试分支。 */
extern rt_u32 amp_test_read32(rt_u32 base, rt_u32 offset);
extern void amp_test_write32(rt_u32 base, rt_u32 offset, rt_u32 value);
extern void amp_test_write16(rt_u32 base, rt_u32 offset, rt_u16 value);
extern rt_u64 amp_test_ticks(void);
#define read32(base, offset) amp_test_read32((base), (offset))
#define write32(base, offset, value) amp_test_write32((base), (offset), (value))
rt_u64 amp_ticks(void) { return amp_test_ticks(); }
#else
static rt_u32 read32(rt_u32 base, rt_u32 offset)
{
    return *(volatile rt_u32 *)(uintptr_t)(base + offset);
}

static void write32(rt_u32 base, rt_u32 offset, rt_u32 value)
{
    *(volatile rt_u32 *)(uintptr_t)(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

rt_u64 amp_ticks(void)
{
    rt_u32 lo, hi;
    __asm__ volatile("isb\nmrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi) :: "memory");
    return ((rt_u64)hi << 32) | lo;
}
#endif

static rt_u64 clock_ns(void *context)
{
    rt_u64 ticks = amp_ticks();
    (void)context;
    return amp_ticks_to_ns(ticks);
}

#if AMP_512_EXPERIMENT
static rt_u64 clock_ticks(void *context)
{
    (void)context;
    return amp_ticks();
}
#endif

/* 停止SGI可以打断300ms复位等待；不在中断内清理共享外设。 */
static int wait_ticks(struct amp_board *board, rt_u32 ticks)
{
    rt_u64 start = amp_ticks(), now;
    do {
        if (*board->stop_requested)
            return RT_REPLY_BAD_STATE;
        now = amp_ticks();
        if (now < start)
            return AMP_ECLOCK;
    } while (now - start < ticks);
    return 0;
}

static void delay_us(void *context, rt_u32 us)
{
    (void)context;
    rt_u64 start = amp_ticks();
    while (amp_ticks() - start < (rt_u64)us * 24U) {}
}

static rt_u32 lbc_read(void *context, rt_u32 offset)
{
    (void)context;
    return read32(LBC, offset);
}

static void lbc_write(void *context, rt_u32 offset, rt_u32 value)
{
    (void)context;
    write32(LBC, offset, value);
}

static void lbc_write16(void *context, rt_u32 offset, rt_u16 value)
{
    (void)context;
#ifdef AMP_BOARD_HOST_TEST
    amp_test_write16(LBC, offset, value);
#else
    *(volatile rt_u16 *)(uintptr_t)(LBC + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

static const struct rt_lbc_ops lbc_ops = {
    .read32 = lbc_read, .write32 = lbc_write, .write16 = lbc_write16,
    .clock_ns = clock_ns, .delay_us = delay_us,
};

static int select_chip(void *context, rt_u32 chip)
{
    struct amp_board *board = context;
    rt_u32 value;
    if (chip > 7U || !board->touched)
        return RT_LBC_EINVAL;
    board->read_chip = chip;
    value = chip << 3;
    /* 保留既有译码器切换方式；CS0瞬态仍须示波器检查，不能称所有CS均关闭。 */
    write32(GPIO, 0x598U, ~value & 0x38U);
    write32(GPIO, 0x594U, value);
    return wait_ticks(board, board->running ? AMP_SELECT_SETTLE_TICKS : 24U);
}

static int busy(void *context)
{
    (void)context;
    return !!(read32(GPIO, 0x90U) & (1U << 4));
}

static int phase(void *context, rt_u32 *value, rt_u32 *period)
{
    struct amp_board *board = context;
    if (!board->running || !(read32(PWM, RT_CONVST_PWM_ENABLE) & RT_CONVST_PWM_ENABLE_MASK))
        return RT_ADC_EWINDOW;
    *period = rt_adc_convst_period(read32(PWM, RT_CONVST_PWM_PERIOD));
    *value = rt_adc_convst_phase(read32(PWM, RT_CONVST_PWM_PHASE));
    if (board->phase_calls < 2U)
        board->read_phase[board->phase_calls] = *value;
    board->phase_calls++;
    return 0;
}

/* 只RMW已取得所有权的复用字段，不写PB8、PB13或整组GPIO/时钟复位。 */
static const struct { rt_u32 offset, mask, value; } pins[] = {
    {0x80U, 0x00ff0000U, 0x00100000U}, /* PA4输入、PA5 RESET输出 */
    {0x84U, 0xffff0000U, 0x66660000U}, /* PA12..15，SDK GPA_LBUS=6 */
    {0x88U, 0x00f0ffffU, 0x00606666U}, /* PA16..19、21 LBUS */
    {0x204U, 0xffff0000U, 0x88880000U}, /* PD12..15，SDK GPD_LBUS=8 */
    {0x208U, 0x00ffffffU, 0x00888888U}, /* PD16..21 LBUS */
    {0x580U, 0x00fff000U, 0x00111000U}, /* PK3..5译码器 */
    {RT_CONVST_GPIO_CFG, RT_CONVST_GPIO_CFG_MASK, 1U << RT_CONVST_GPIO_CFG_SHIFT},
};

int amp_board_prepare(void *context, const struct rt_adc_register_plan *plan)
{
    struct amp_board *board = context;
    static const rt_u32 timing[] = {
        0x02011000U, 0, 0x00c10021U, 0x101U, 0x101U, 0x04000100U,
        0x09090000U, 1, 0, 0x00060200U, 0x00070200U, 0, 1, 0x000c000aU,
        0x00060004U, 0, 7, 0x11U, 0, 0, 0,
    };
    rt_u32 i, chip, clock = read32(CCU, 0x1040U);
    rt_u16 actual, command;
    int result;
    /* Linux配套必须先用原生clock框架持有资源；A7绝不自行打开共享PLL/CCU gate。 */
    if (board->touched ||
        (clock & 0x87000000U) != 0x81000000U ||
        (((clock >> 8) & 31U) + 1U) * ((clock & 31U) + 1U) != 4U ||
        (read32(CCU, 0x104cU) & 0x10001U) != 0x10001U ||
        !(read32(CCU, 0x5e4U) & (1U << 27)) ||
        (read32(CCU, 0x794U) & 0x10001U) != 0x10001U ||
        !rt_adc_convst_clock_group_available(read32(PWM, RT_CONVST_PWM_ENABLE)) ||
        read32(GPIO, 0xd0U) || /* SDK GPIO布局/既有端口PA EINT_CTRL，拒绝并行IRQ源 */
        (read32(LBC, 0x518U) & 0x01ff1ff7U))
        return AMP_EOWNER;
    board->period = plan->period_ticks;
    board->mask = plan->enabled_chip_mask;
    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        board->saved[i] = read32(GPIO, pins[i].offset);
    /* 先置RESET高、CONVST低和译码器111，再改输出复用，避免窄复位/触发脉冲。 */
    write32(GPIO, 0x94U, 1U << 5);
    write32(GPIO, 0x594U, 0x38U);
    write32(GPIO, RT_CONVST_GPIO_DATA + 8U, 1U << 10);
    board->touched = 1;
    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        write32(GPIO, pins[i].offset,
                  (read32(GPIO, pins[i].offset) & ~pins[i].mask) | pins[i].value);
        if ((read32(GPIO, pins[i].offset) & pins[i].mask) != pins[i].value)
            return RT_LBC_EIO;
    }
    write32(LBC, 0x500U, 0);
    write32(LBC, 0x504U, 0);
    for (chip = 0; chip < 4; chip++) {
        for (i = 0; i < sizeof(timing) / sizeof(timing[0]); i++)
            write32(LBC, chip * 0x80U + i * 4U, timing[i]);
        write32(LBC, chip * 0x80U + 0x60U, 2);
        write32(LBC, 0x210U + chip * 16U, chip * 0x10000000U + 0x0fffffffU);
        write32(LBC, 0x214U + chip * 16U, chip * 0x10000000U);
        write32(LBC, 0x218U + chip * 16U, chip * 0x10000000U + 0x0fffffffU);
        write32(LBC, 0x21cU + chip * 16U, chip * 0x10000000U);
    }
    write32(LBC, 0x200U, 1);
    write32(LBC, 0x250U, 0x10000U);
    write32(LBC, 0x280U, 0x10005U);
    write32(LBC, 0x290U, 0x10000U);
    write32(LBC, 0x440U, 0x111U);
#if ADC_LBC_MODE == ADC_LBC_DA
    /* SDK DA例程：CPUX窗口c0000000+内部CS3偏移30000000，AXI决定长度。
     * 外部74HC138片选仍用PK3/4/5；配置ADC寄存器继续使用原IDA路径。 */
    write32(LBC, 0x400U, 0);
    write32(LBC, 0x404U, 0);
    for (chip = 0; chip < 4; chip++) write32(LBC, 0x410U + chip * 4U, 0);
    write32(LBC, 0x524U, 0x101U);
#endif
    /* 仅遥信模式只配置共用总线，既不复位/配置ADC，也不启动CONVST。 */
    if (AMP_BUS_DI_ONLY) return select_chip(board, 7);
    write32(GPIO, 0x98U, 1U << 5);
    result = wait_ticks(board, 480U);
    write32(GPIO, 0x94U, 1U << 5);
    if (!result)
        result = wait_ticks(board, 7200000U);
    if (result)
        return result;
    /* 五片共用RESET/PWM且仅观察一路BUSY。单片读测试也配置五片相同序列，
       避免未配置的BUSY来源片先完成单通道转换，导致对另一片提前读取。 */
    for (chip = 0; chip < RT_ADC_CHIPS; chip++) {
        result = select_chip(board, chip);
        if (result)
            return result;
        result = rt_lbc_write_register(&lbc_ops, 2, plan->mode[chip], &actual);
        for (i = 0; !result && i < 4; i++)
            result = rt_lbc_write_register(&lbc_ops, (rt_u8)(4 + i), plan->range[chip][i], &actual);
        for (i = 0; !result && i < 8; i++)
            result = rt_lbc_write_register(&lbc_ops, (rt_u8)(0x20U + i),
                         (rt_u16)(i * 0x11U | (i == 7 ? 0x100U : 0)), &actual);
        if (result)
            return result;
        command = 0;
        result = rt_lbc_transfer(&lbc_ops, 0, &command, 1);
        if (result)
            return result;
    }
    return select_chip(board, 7);
}

int amp_board_start(void *context, rt_u32 period)
{
    struct amp_board *board = context;
    rt_u32 value, chip;
    if (!board->touched || board->running || period != board->period ||
        !rt_adc_convst_clock_group_available(read32(PWM, RT_CONVST_PWM_ENABLE)))
        return RT_LBC_EBUSY;
    for (chip = 0; chip < 4; chip++)
        write32(LBC, chip * 0x80U + 0x34U, 0x0009000aU);
    if (AMP_BUS_DI_ONLY) {
        write32(LBC, 0x508U, ~0U);
        write32(LBC, 0x50cU, ~0U);
        write32(LBC, 0x304U, 0x30000000U);
        write32(LBC, 0x308U, 1U);
        board->running = 1;
        return 0;
    }
#if ADC_LBC_MODE == ADC_LBC_IDA_PIO && AMP_IDA_FIXED_READ
    /* 配置阶段已完成最后一笔ADC寄存器访问。运行期间只有32字节读，
     * 将固定地址/长度和历史标志清理移到PWM启用前；每片仍独立START并检查完成。 */
    if (read32(LBC, 0x518U) & 0x01ff1ff7U) return RT_LBC_EBUSY;
    write32(LBC, 0x508U, ~0U);
    write32(LBC, 0x50cU, ~0U);
    write32(LBC, 0x304U, 0x30000000U);
    write32(LBC, 0x308U, 31U);
#endif
    write32(GPIO, RT_CONVST_GPIO_CFG,
        (read32(GPIO, RT_CONVST_GPIO_CFG) & ~RT_CONVST_GPIO_CFG_MASK) |
        (RT_CONVST_GPIO_MUX << RT_CONVST_GPIO_CFG_SHIFT));
    write32(PWM, RT_CONVST_PWM_CLOCK, read32(PWM, RT_CONVST_PWM_CLOCK) & ~((3U << 7) | 0xfU));
    value = read32(PWM, RT_CONVST_PWM_GATE);
    write32(PWM, RT_CONVST_PWM_GATE, (value & ~(RT_CONVST_PWM_ENABLE_MASK << 16)) | RT_CONVST_PWM_ENABLE_MASK);
    value = read32(PWM, RT_CONVST_PWM_CONTROL) &
        ~(0xffU | (1U << 9) | (1U << 10) | (1U << 12) | 0xffff0000U);
    write32(PWM, RT_CONVST_PWM_CONTROL, value | (1U << 8));
    write32(PWM, RT_CONVST_PWM_PERIOD, rt_adc_convst_period_value(24U, period));
    write32(PWM, RT_CONVST_PWM_PHASE, 0);
    board->running = 1;
    write32(PWM, RT_CONVST_PWM_ENABLE, read32(PWM, RT_CONVST_PWM_ENABLE) | RT_CONVST_PWM_ENABLE_MASK);
    {
        /* PCNTR低半是只读当前计数，写0不能保证重启时清零；改短周期后旧计数可能超界。
         * 不读取ADC、不计资格，先等待真实回绕并测到一个符合新配置的完整PWM周期。 */
        rt_u64 started = amp_ticks(), last_wrap = 0;
        rt_u32 previous = rt_adc_convst_phase(read32(PWM, RT_CONVST_PWM_PHASE));
        for (;;) {
            rt_u32 current = rt_adc_convst_phase(read32(PWM, RT_CONVST_PWM_PHASE));
            rt_u64 now = amp_ticks();
            if (*board->stop_requested || now - started > 3U * 65536U)
                return AMP_ECLOCK;
            if (current < previous) {
                rt_u64 interval = now - last_wrap;
                if (last_wrap && interval + AMP_POLL_GUARD_TICKS >= period &&
                    interval <= period + AMP_POLL_GUARD_TICKS && current < period)
                    break;
                last_wrap = now;
            }
            previous = current;
        }
    }
    return 0;
}

int amp_board_stop(void *context)
{
    struct amp_board *board = context;
    rt_u32 i;
    if (!board->touched)
        return 0;
    write32(PWM, RT_CONVST_PWM_ENABLE, read32(PWM, RT_CONVST_PWM_ENABLE) & ~RT_CONVST_PWM_ENABLE_MASK);
    board->running = 0;
    if (read32(PWM, RT_CONVST_PWM_ENABLE) & RT_CONVST_PWM_ENABLE_MASK)
        return RT_LBC_EIO;
    /* 不再发转换，且等待最后一次真实BUSY结束后才归还引脚。 */
    {
        rt_u64 start = amp_ticks();
        while (busy(board))
            if (amp_ticks() - start > 480U)
                return RT_ADC_EBUSY_PIN;
    }
    /* IDA超时后控制器可能仍在事务中；未确认空闲时禁止归还引脚和时钟。 */
    if ((read32(LBC, 0x518U) & 0x01ff1ff7U) ||
        (read32(LBC, 0x514U) & 0x007f1f07U))
        return RT_LBC_EBUSY;
    write32(GPIO, 0x594U, 0x38U);
    write32(GPIO, RT_CONVST_GPIO_DATA + 8U, 1U << 10);
    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        write32(GPIO, pins[i].offset,
            (read32(GPIO, pins[i].offset) & ~pins[i].mask) | (board->saved[i] & pins[i].mask));
        if ((read32(GPIO, pins[i].offset) & pins[i].mask) != (board->saved[i] & pins[i].mask))
            return RT_LBC_EIO;
    }
    board->touched = 0;
    return 0;
}

struct amp_observe_failure amp_observe_failure;

int amp_board_read_digital(void *context, rt_u32 *raw, rt_u32 *duration_ticks)
{
    struct amp_board *board = context;
    rt_u16 word = 0;
    rt_u64 started = amp_ticks();
    rt_u32 status, before = 0;
    int result = 0;
    if (!board || !board->running || !raw || !duration_ticks) return RT_LBC_EINVAL;
    *duration_ticks = 0;
    if (read32(LBC, 0x518U) & 0x01ff1ff7U) return RT_LBC_EBUSY;
    if (!AMP_BUS_DI_ONLY) {
        before = rt_adc_convst_phase(read32(PWM, RT_CONVST_PWM_PHASE));
        /* 原ADC读窗及后台余量不变，遥信不得把总事务拖到下一次转换。 */
        if (before >= board->period || before + 1440U >= board->period)
            return RT_ADC_EWINDOW;
        write32(LBC, 0x304U, 0x30000000U);
        write32(LBC, 0x308U, 1U);
    }
    do {
        result = select_chip(board, AMP_BUS_DI_CHIP);
        if (result) break;
        write32(LBC, 0x508U, 8U);
        write32(LBC, 0x300U, 0x00038001U);
        while (!((read32(LBC, 0x518U) >> 16) & 0x1ffU)) {
            if (*board->stop_requested || amp_ticks() - started >= 480U) {
                result = RT_LBC_ETIMEOUT;
                break;
            }
        }
        if (result) break;
        word = (rt_u16)read32(LBC, 0x350U);
        do {
            status = read32(LBC, 0x508U);
            if ((status & 2U) || (read32(LBC, 0x50cU) & 0x74U)) {
                result = RT_LBC_EIO;
                break;
            }
            if (*board->stop_requested || amp_ticks() - started >= 480U) {
                result = RT_LBC_ETIMEOUT;
                break;
            }
        } while (!(status & 8U));
        if (result) break;
        if (read32(LBC, 0x518U) & 0x01ff1ff7U) {
            result = RT_LBC_EBUSY;
            break;
        }
    } while (0);
    /* 下一笔ADC必须恢复32字节；失败且仍忙时禁止继续写事务配置。 */
    if (!AMP_BUS_DI_ONLY && !result) write32(LBC, 0x308U, 31U);
    *duration_ticks = (rt_u32)(amp_ticks() - started);
    if (!result && !AMP_BUS_DI_ONLY) {
        rt_u32 after = rt_adc_convst_phase(read32(PWM, RT_CONVST_PWM_PHASE));
        if (after < before || after >= board->period || busy(board)) result = RT_ADC_EWINDOW;
    }
    if (!result) *raw = word;
    return result;
}

int amp_board_observe(struct amp_board *board, struct amp_observation *out)
{
    rt_u64 started, end;
    rt_u32 reason;
    /* 周期由独占配置固定，每帧总线读前后另核真实PWM周期。轮询只做
     * GPIO→计时→相位两笔APB，避免五笔寄存器访问遮住窄BUSY高电平。
     * BUSY先于相位：跨回绕时状态机先清本周期资格，不能拿旧低电平读ADC。 */
    out->period = board->period;
    if (!board->running) {
        amp_observe_failure = (struct amp_observe_failure){1, 0, 0, 0, out->period};
        out->ticks = amp_ticks(); out->phase = 0;
        return AMP_ECLOCK;
    }
    started = amp_ticks();
    out->busy = busy(0);
    out->ticks = amp_ticks();
    out->phase = rt_adc_convst_phase(read32(PWM, RT_CONVST_PWM_PHASE));
    end = amp_ticks();
    if (end >= started && end - started <= AMP_POLL_GUARD_TICKS &&
        out->phase < out->period && out->phase > end - started)
        return 0;
    reason = end < started ? 2U : (end - started > AMP_POLL_GUARD_TICKS ? 3U :
        (out->phase >= out->period ? 4U : 5U));
    amp_observe_failure = (struct amp_observe_failure){reason,
        (rt_u32)(end - started), out->phase, out->phase, out->period};
    /* 相位小于本次访问时长时，BUSY可能属于回绕前一周期，必须重取。
     * 总线暂时变慢/跨回绕只表示此次快照不可用。调用者不得拿它读ADC，
     * 必须在原周期连续性边界内重读；真实倒退仍立即撤源。 */
    return reason == 2U ? AMP_ECLOCK : AMP_OBSERVE_RETRY;
}

#if ADC_LBC_MODE == ADC_LBC_DA
static int read_direct(void *context, rt_u16 *data, rt_u32 words)
{
    rt_u32 i;
    struct amp_board *board = context;
    if (!board->running || words != 16U) return RT_LBC_EINVAL;
    /* SDK sunxi_lbc_da_read的32bit AXI访问，在16bit LBC上拆成两个数据拍。
     * Device/XN映射防止预取/缓存；每片恰好8次32bit，共16个16bit码。 */
    for (i = 0; i < words; i += 2U) {
        rt_u32 value = read32(DA_CS3, i * 2U);
        data[i] = (rt_u16)value;
        data[i + 1U] = (rt_u16)(value >> 16);
    }
    if ((read32(LBC, 0x508U) & 0x0ff00082U) ||
        (read32(LBC, 0x50cU) & 0x14U) ||
        (read32(LBC, 0x514U) & 0x007f1f07U)) return RT_LBC_EIO;
    return RT_LBC_OK;
}
#endif

#if ADC_LBC_MODE == ADC_LBC_IDA_PIO && AMP_IDA_FIXED_READ
static int read_ida_fixed(void *context, rt_u16 *data, rt_u32 words)
{
    rt_u32 i, end, status;
    rt_u64 started = amp_ticks();
    struct amp_board *board = context;
    if (!board->running || words != 16U) return RT_LBC_EINVAL;
    board->read_stage = 4;
    /* 本帧五片共用固定地址/长度；上一片必须完成后才允许下一次片选。 */
    write32(LBC, 0x508U, 8U);
    write32(LBC, 0x300U, 0x00038001U);
    /* 每4个32bit字搬一次，使前半FIFO读取与后半RD重叠。
     * 两段均先检查可用量，仍只取16个16bit字，完整完成后才切下一片。 */
    for (end = 8U; end <= words; end += 8U) {
        while (((read32(LBC, 0x518U) >> 16) & 0x1ffU) < 4U) {
            if (*board->stop_requested || amp_ticks() - started >= 480U) return RT_LBC_ETIMEOUT;
        }
        board->read_stage = 5;
        for (i = end - 8U; i < end; i += 2U) {
            rt_u32 value = read32(LBC, 0x350U);
            data[i] = (rt_u16)value;
            data[i + 1U] = (rt_u16)(value >> 16);
        }
    }
    do {
        status = read32(LBC, 0x508U);
        if (status & 2U) return RT_LBC_EIO;
        if (*board->stop_requested || amp_ticks() - started >= 480U) return RT_LBC_ETIMEOUT;
    } while (!(status & 8U));
    board->read_stage = 6;
    if (read32(LBC, 0x518U) & 0x01ff1ff7U) return RT_LBC_EBUSY;
    return 0;
}
#endif

int amp_board_read(void *context, const struct rt_adc_busy_event *event,
                     rt_s16 analog[RT_ANALOG_CHANNELS], rt_u32 *mask, rt_u64 *read_ns)
{
    struct amp_board *board = context;
    struct rt_adc_frame_ops ops = {
        .lbc = &lbc_ops, .context = board, .select_chip = select_chip,
        .busy = busy, .phase = phase, .clock_ns = clock_ns,
        .start_from_busy_event = AMP_512_EXPERIMENT,
#if AMP_512_EXPERIMENT
        .clock_ticks = clock_ticks,
#endif
#if ADC_LBC_MODE == ADC_LBC_DA
        .read_selected = read_direct,
#elif AMP_IDA_FIXED_READ
        .read_selected = read_ida_fixed,
#endif
    };
    rt_u32 start_phase;
    rt_u64 started;
    int result;
    if (!board || !event || !analog || !mask || !read_ns) return RT_LBC_EINVAL;
    started = amp_ticks();
    board->phase_calls = 0;
    board->read_phase[0] = board->read_phase[1] = 0;
    board->read_stage = 1;
    /* 固定配置的前置检查同样不得遗留上一帧有效位。 */
    *mask = 0;
    *read_ns = 0;
#if ADC_LBC_MODE == ADC_LBC_IDA_PIO && AMP_IDA_FIXED_READ
    if (read32(LBC, 0x518U) & 0x01ff1ff7U) {
        result = RT_LBC_EBUSY;
        goto done;
    }
#endif
    board->read_stage = 2;
    result = rt_adc_read_frame_on_busy(&ops, analog, board->period, board->mask,
                                      event, mask, &start_phase, read_ns);
    board->start_bound = start_phase;
    if (!result && ((read32(LBC, 0x508U) & 0x0ffff0f2U) ||
                    (read32(LBC, 0x50cU) & 0x74U))) {
        *mask = 0;
        result = RT_LBC_EIO;
    }
#if ADC_LBC_MODE == ADC_LBC_IDA_PIO && AMP_IDA_FIXED_READ
done:
#endif
    board->frame_result = result;
    board->frame_last_ticks = (rt_u32)(amp_ticks() - started);
    if (!result) {
        board->frame_count++;
        board->frame_sum_ticks += board->frame_last_ticks;
        if (board->frame_last_ticks > board->frame_max_ticks)
            board->frame_max_ticks = board->frame_last_ticks;
        board->read_stage = 7;
    }
    return result;
}
