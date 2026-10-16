#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "../firmware/platform.h"

static uint32_t gpio[0x600 / 4], lbc[0x600 / 4];
static uint32_t tick, transfers, gpio_writes, cs_writes;
static int receive_active, stalled;
enum {
    TEST_READ_COUNT = 10000U,
    TEST_PROBE_LOW_BYTE = 0x5aU,
    TEST_PROBE_HIGH_BYTE = 0xa5U,
    TEST_DATA_PIN_SHIFT = 12U,
    TEST_PROBE_WORD = 0xa55aU
}; /* 足够覆盖重复 RX 与 CS 保持不变的行为。 */

uint64_t l16_ticks(void) {
    return ++tick;
}

uint32_t l16_read32(uint32_t address)
{
    if (address == 0x02003040U) return 0x81000003U;
    if (address == 0x0200304cU) return 0x10001U;
    if (address >= 0x03604000U && address < 0x03604600U) return gpio[(address - 0x03604000U) / 4];
    if (address == 0x02810518U) return receive_active && !stalled ? 1U << 16 : 0;
    if (address == 0x02810350U) {
        receive_active = 0;
        return 0x8001U;
    }
    /* 实机 DONE 必须在读取 RX 尾字后才出现，防止错误的轮询次序回归。 */
    if (address == 0x02810508U) return stalled ? 0 : (receive_active ? 1U : 8U);
    if (address >= 0x02810000U && address < 0x02810600U) return lbc[(address - 0x02810000U) / 4];
    assert(!"unexpected MMIO read");
    return 0;
}

void l16_write32(uint32_t address, uint32_t value)
{
    if (address >= 0x03604000U && address < 0x03604600U) {
        uint32_t offset = address - 0x03604000U;
        gpio_writes++;
        /* 数据寄存器禁止整字写；CS/WR 的置清也不能包含数据位。 */
        assert(offset != 0x90U && offset != 0x210U && offset != 0x590U);
        if (offset == 0x94U || offset == 0x98U) {
            assert(value == 1U << 21);
            cs_writes++;
        }
        if (offset == 0x214U || offset == 0x218U) assert(value == 1U << 20);
        gpio[offset / 4] = value;
    } else {
        assert(address >= 0x02810000U && address < 0x02810600U);
        assert(address != 0x02810340U);
        if (address == 0x02810300U) {
            assert(value == 0x00038001U);
            receive_active = 1;
            transfers++;
        }
        if (address != 0x0281050cU) lbc[(address - 0x02810000U) / 4] = value;
    }
}

int main(void)
{
    uint32_t i, initial_gpio, initial_cs;
    uint16_t word = 0;
    volatile uint32_t stop = 0;
    assert(sizeof(struct l16_status) == 144);
    assert(offsetof(struct l16_status, command_gen) == 96);
    assert(L16_SAMPLE_ADDRESS + L16_SAMPLE_BYTES <= 0x44400000U);
    assert(l16_capacity(24) == 1000000 && l16_capacity(240) == 100000);
    assert(l16_capacity(24000) == 1000 && l16_capacity(0) == 0);
    assert(l16_pattern(500247) == 0x510b && l16_pattern(500248) == 0x510c);
    assert(l16_pattern(500251) == 0x510c && l16_pattern(500252) == 0x510f);
    assert(l16_board_prepare() == 0);
    assert((gpio[0x84 / 4] & 0xffff0000U) == 0x66660000U);
    assert((gpio[0x204 / 4] & 0xffff0000U) == 0x88880000U);
    assert((gpio[0x88 / 4] & 0x00f0ffffU) == 0x00106666U);
    assert((gpio[0x208 / 4] & 0x00ffffffU) == 0x00818888U);
    initial_gpio = gpio_writes;
    initial_cs = cs_writes;
    for (i = 0; i < TEST_READ_COUNT; i++) {
        assert(l16_board_read(&word, &stop) == 0);
        assert(word == 0x8001U);
    }
    assert(transfers == 10000 && gpio_writes == initial_gpio && cs_writes == initial_cs);
    gpio[0x90U / 4] = TEST_PROBE_HIGH_BYTE << TEST_DATA_PIN_SHIFT;
    gpio[0x210U / 4] = TEST_PROBE_LOW_BYTE << TEST_DATA_PIN_SHIFT;
    assert(l16_board_probe_inputs() == TEST_PROBE_WORD);
    assert(cs_writes == initial_cs && transfers == TEST_READ_COUNT);
    assert((gpio[0x84U / 4] & 0xffff0000U) == 0x66660000U);
    assert((gpio[0x204U / 4] & 0xffff0000U) == 0x88880000U);
    assert((gpio[0x88U / 4] & 0x00f0ffffU) == 0x00106666U);
    assert((gpio[0x208U / 4] & 0x00ffffffU) == 0x00818888U);
    assert(l16_board_check() == 0);
    lbc[0x50cU / 4] = 0x20U;
    assert(l16_board_check() == L16_BUS_ERROR);
    lbc[0x50cU / 4] = 0;
    stalled = 1;
    tick = 0;
    assert(l16_board_read(&word, &stop) == L16_READ_TIMEOUT && tick <= 482);
    stalled = 0;
    receive_active = 1;
    assert(l16_board_restore() == 0);
    assert(receive_active == 0 && transfers == 10001);
    puts("PASS: 16-bit RX, 10,000 reads without CS/GPIO writes; input probe preserves mux and CS; bounded timeout and layout");
    return 0;
}
