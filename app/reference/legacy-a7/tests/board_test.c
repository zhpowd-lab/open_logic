#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "rt_acquisition_target.h"

static rt_u32 gpio[1024], lbc[1024], ccu[2048], pwm[256];
static rt_u16 registers[5][64];
static rt_u64 ticks, pwm_start;
static rt_u32 writes, ccu_writes, receive_remaining, read_register, data_index, sample_words;
static rt_u32 running_config_writes;
static rt_u32 di_reads, adc_register_writes, reset_low;
static rt_u32 apb_ticks = 1;
static int stalled, lbc_busy;
static volatile rt_u32 stop_request;
static rt_u32 *space(rt_u32 base)
{
    switch (base) {
    case 0x03604000U: return gpio;
    case 0x02810000U: return lbc;
    case 0x02002000U: return ccu;
    case 0x020c0000U: return pwm;
    default: assert(!"unexpected peripheral address"); return NULL;
    }
}
rt_u64 amp_test_ticks(void) { ticks += 3; return ticks; }
rt_u32 amp_test_read32(rt_u32 base, rt_u32 offset)
{
    rt_u32 *regs, chip = (gpio[0x590 / 4] >> 3) & 7U;
    if (base == 0xf0000000U) {
        rt_u32 value;
        assert(chip < 5 && offset < 32U && !(offset & 3U));
        ticks++;
        value = (rt_u16)(-12000 + (int)chip * 1000 + (int)offset / 2);
        value |= (rt_u32)(rt_u16)(-11999 + (int)chip * 1000 + (int)offset / 2) << 16;
        sample_words += 2;
        return value;
    }
    regs = space(base);
    ticks += apb_ticks;
    assert(!(offset & 3));
    if (base == 0x02810000U) {
        if (offset == 0x518) return lbc_busy ? 1U : (stalled ? 0 : ((receive_remaining + 1U) / 2U) << 16);
        if (offset == 0x508) return 8;
        if (offset == 0x50c) return stalled && ADC_LBC_MODE == ADC_LBC_DA ? 0x10U : 0;
        if (offset == 0x350) {
            rt_u32 value;
            assert(chip < 7 && receive_remaining);
            if (chip >= 5) {
                assert(chip == 5 && lbc[0x308 / 4] == 1);
                value = 0xa55aU;
                di_reads++;
            } else if ((lbc[0x308 / 4] + 1) == 2)
                value = registers[chip][read_register];
            else {
                value = (rt_u16)(-12000 + (int)chip * 1000 + (int)data_index);
                value |= (rt_u32)(rt_u16)(-11999 + (int)chip * 1000 + (int)data_index) << 16;
                data_index += 2;
                sample_words += 2;
            }
            receive_remaining -= receive_remaining >= 2 ? 2 : 1;
            return value;
        }
    }
    if (base == 0x020c0000U && offset == 0x388 && (pwm[0x80 / 4] & (1U << 10)))
        return (rt_u32)((ticks - pwm_start) % ((pwm[0x384 / 4] >> 16) + 1U));
    return regs[offset / 4];
}
void amp_test_write32(rt_u32 base, rt_u32 offset, rt_u32 value)
{
    rt_u32 *regs = space(base);
    ticks++; writes++;
    if (base == 0x02810000U && (offset == 0x304U || offset == 0x308U) &&
        (pwm[0x80U / 4] & (1U << 10)))
        running_config_writes++;
    if (base == 0x02002000U) ccu_writes++;
    if (base == 0x03604000U) {
        if (offset == 0x98 && (value & (1U << 5))) reset_low++;
        if (offset == 0x94 || offset == 0x594 || offset == 0x514)
            regs[(offset - 4) / 4] |= value;
        if (offset == 0x98 || offset == 0x598 || offset == 0x518)
            regs[(offset - 8) / 4] &= ~value;
    }
    if (base == 0x02810000U && offset == 0x300) {
        receive_remaining = value & 2 ? 0 : (lbc[0x308 / 4] + 1) / 2;
        data_index = 0;
    }
    if (base == 0x020c0000U && offset == 0x80 && (value & (1U << 10)))
        pwm_start = ticks;
    regs[offset / 4] = value;
}
void amp_test_write16(rt_u32 base, rt_u32 offset, rt_u16 value)
{
    rt_u32 chip = (gpio[0x590 / 4] >> 3) & 7U;
    assert(base == 0x02810000U && offset == 0x340 && chip < 5);
    adc_register_writes++;
    ticks++;
    read_register = (value >> 9) & 63U;
    if (value & 0x8000)
        registers[chip][read_register] = value & 0x1ffU;
}
static void init(void)
{
    memset(gpio, 0, sizeof(gpio)); memset(lbc, 0, sizeof(lbc));
    memset(ccu, 0, sizeof(ccu)); memset(pwm, 0, sizeof(pwm));
    memset(registers, 0, sizeof(registers));
    ticks = 100000; receive_remaining = writes = ccu_writes = sample_words = 0;
    running_config_writes = 0;
    di_reads = adc_register_writes = reset_low = 0;
    apb_ticks = 1;
    stalled = lbc_busy = 0; stop_request = 0;
    ccu[0x1040 / 4] = 0x81000003U;
    ccu[0x104c / 4] = ccu[0x794 / 4] = 0x10001;
    ccu[0x5e4 / 4] = 1U << 27;
}
int main(void)
{
    struct amp_board board = {.stop_requested = &stop_request};
    struct rt_adc_register_plan plan = {.generation = 1, .period_ticks = 3750, .enabled_chip_mask = 31};
    struct amp_observation observed;
    struct rt_adc_busy_event event;
    rt_s16 analog[80];
    rt_u32 chip, i, mask;
    rt_u64 read_ns;
    rt_u32 di_raw = 0, di_ticks = 0;
    for (chip = 0; chip < 5; chip++) {
        plan.mode[chip] = 0x60;
        for (i = 0; i < 4; i++) plan.range[chip][i] = 0xff;
    }
    init();
    ccu[0x1040 / 4] = 0;
    assert(amp_board_prepare(&board, &plan) == AMP_EOWNER);
    assert(!writes && !board.touched);
    assert(!amp_board_stop(&board) && !writes);
    init();
    assert(!amp_board_prepare(&board, &plan));
    if (AMP_BUS_DI_ONLY) {
        assert(!adc_register_writes && !sample_words && !reset_low);
        assert(!amp_board_start(&board, 3750));
        assert(!(pwm[0x80 / 4] & (1U << 10)));
        assert(!amp_board_read_digital(&board, &di_raw, &di_ticks));
        assert(di_raw == 0xa55aU && di_reads == 1 && di_ticks > 0);
        assert(!sample_words && !adc_register_writes && !reset_low);
        lbc_busy = 1;
        assert(amp_board_read_digital(&board, &di_raw, &di_ticks) == RT_LBC_EBUSY);
        lbc_busy = 0; stalled = 1;
        di_raw = 0x12345678U;
        assert(amp_board_read_digital(&board, &di_raw, &di_ticks) == RT_LBC_ETIMEOUT);
        assert(di_raw == 0x12345678U);
        stalled = 0; receive_remaining = 0;
        assert(!amp_board_stop(&board) && !board.touched);
        puts("{\"di_only\":true,\"adc_transactions\":0,\"pwm_started\":false,\"result\":\"passed\"}");
        return 0;
    }
    assert((gpio[0x84 / 4] & 0xffff0000U) == 0x66660000U);
    assert((gpio[0x208 / 4] & 0x00ffffffU) == 0x00888888U);
    assert(!ccu_writes);
    for (chip = 0; chip < 5; chip++) {
        assert(registers[chip][2] == 0x60);
        for (i = 0; i < 4; i++) assert(registers[chip][i + 4] == 0xff);
        for (i = 0; i < 8; i++) assert(registers[chip][i + 0x20] == (i * 0x11U | (i == 7 ? 0x100U : 0)));
    }
    assert(!amp_board_start(&board, 3750));
    /* 回绕期间的慢APB快照仍须在原4µs内重取；失效使能和真超时不能放行。 */
    apb_ticks = 10;
    ticks = pwm_start + 3750 - 40;
    assert(!amp_board_observe(&board, &observed) && observed.phase < 3750);
    ticks = pwm_start + 3750 - 12;
    assert(amp_board_observe(&board, &observed) == AMP_OBSERVE_RETRY);
    assert(amp_observe_failure.reason == 5);
    apb_ticks = 100;
    assert(amp_board_observe(&board, &observed) == AMP_OBSERVE_RETRY);
    assert(amp_observe_failure.reason == 3 && amp_observe_failure.elapsed > 96);
    apb_ticks = 1;
    board.running = 0;
    assert(amp_board_observe(&board, &observed) == AMP_ECLOCK && amp_observe_failure.reason == 1);
    board.running = 1;
    ticks = pwm_start + 190;
    assert(!amp_board_observe(&board, &observed));
    event.phase = observed.phase;
    event.clock_ns = observed.ticks / 3U * 125U + observed.ticks % 3U * 125U / 3U;
    assert(!amp_board_read_digital(&board, &di_raw, &di_ticks));
    assert(di_raw == 0xa55aU && di_reads == 1 && di_ticks > 0);
    assert(lbc[0x308 / 4] == 31);
    assert(!amp_board_read(&board, &event, analog, &mask, &read_ns));
    assert(mask == 31 && sample_words == 80 && read_ns > 0);
    assert(board.frame_count == 1 && board.phase_calls == 2 && board.frame_result == 0);
    assert(board.frame_max_ticks == board.frame_last_ticks && board.frame_sum_ticks == board.frame_last_ticks);
#if AMP_IDA_FIXED_READ
    assert(running_config_writes == 3);
#endif
    for (i = 0; i < 80; i++)
        assert(analog[i] == (rt_s16)(-12000 + (int)(i / 16) * 1000 + (int)(i % 16)));
    /* 未空闲总线拒绝下一帧，输出有效位不能继承上一帧。 */
    lbc_busy = 1;
    assert(amp_board_read(&board, &event, analog, &mask, &read_ns) == RT_LBC_EBUSY);
    assert(mask == 0);
    lbc_busy = 0;
    assert(!amp_board_stop(&board) && !board.touched && !(pwm[0x80 / 4] & (1U << 10)));
    assert(!ccu_writes);
    init();
    plan.enabled_chip_mask = 4;
    assert(!amp_board_prepare(&board, &plan));
    for (chip = 0; chip < 5; chip++)
        assert(registers[chip][2] == 0x60 && registers[chip][0x27] == 0x177);
    assert(!amp_board_start(&board, 3750));
    ticks = pwm_start + 190;
    assert(!amp_board_observe(&board, &observed));
    event.phase = observed.phase;
    event.clock_ns = observed.ticks / 3U * 125U + observed.ticks % 3U * 125U / 3U;
    assert(!amp_board_read(&board, &event, analog, &mask, &read_ns));
    assert(mask == 4 && sample_words == 16);
    for (i = 0; i < 80; i++)
        assert(analog[i] == (i / 16 == 2 ? (rt_s16)(-10000 + (int)(i % 16)) : 0));
    assert(!amp_board_stop(&board));
    plan.enabled_chip_mask = 31;
    init();
    assert(!amp_board_prepare(&board, &plan));
    assert(!amp_board_start(&board, 3750));
    ticks = pwm_start + 190;
    assert(!amp_board_observe(&board, &observed));
    event.phase = observed.phase;
    event.clock_ns = observed.ticks / 3U * 125U + observed.ticks % 3U * 125U / 3U;
    stalled = 1;
    for (i = 0; i < 80; i++) analog[i] = 1234;
    assert(amp_board_read(&board, &event, analog, &mask, &read_ns) ==
           (ADC_LBC_MODE == ADC_LBC_DA ? RT_LBC_EIO : RT_LBC_ETIMEOUT));
    for (i = 0; i < 80; i++) assert(analog[i] == 1234);
    gpio[0x90 / 4] |= 1U << 4;
    assert(amp_board_stop(&board) == RT_ADC_EBUSY_PIN && board.touched);
    gpio[0x90 / 4] &= ~(1U << 4);
    lbc_busy = 1;
    assert(amp_board_stop(&board) == RT_LBC_EBUSY && board.touched);
    lbc_busy = 0;
    assert(!amp_board_stop(&board));
    /* 高档的BUSY已结束也不能压缩余量；读窗不足时应在第一个RD之前拒绝。 */
    init();
    plan.period_ticks = 1250;
    assert(!amp_board_prepare(&board, &plan));
    assert(!amp_board_start(&board, 1250));
    ticks = pwm_start + 190;
    assert(!amp_board_observe(&board, &observed));
    event.phase = observed.phase;
    event.clock_ns = observed.ticks / 3U * 125U + observed.ticks % 3U * 125U / 3U;
    for (i = 0; i < 80; i++) analog[i] = 1234;
    assert(amp_board_read(&board, &event, analog, &mask, &read_ns) == RT_ADC_EWINDOW);
    assert(mask == 0 && sample_words == 0);
    assert(board.phase_calls == 1 && board.read_stage == 2 && board.frame_result == RT_ADC_EWINDOW);
    for (i = 0; i < 80; i++) assert(analog[i] == 1234);
    assert(!amp_board_stop(&board));
    puts("{\"port_scenarios\":9,\"real_adc\":false,\"result\":\"passed\"}");
    return 0;
}
