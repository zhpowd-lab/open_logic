#include "platform.h"
#include "board-registers.h"

static uint32_t saved[L16_PIN_CONFIG_COUNT], saved_pa_data, saved_pd_data, saved_pk_data;
static int prepared;

/* PA12..19=D8..15，PD12..19=D0..7，PD21=RD；不配置任何数据位为 GPIO 输出。 */
static const struct {
    uint32_t offset, mask, value;
} pins[] = {
    {
        L16_PA_CFG1, L16_DATA_CFG1_MASK, L16_PA_CFG1_RX
    },
    {
        L16_PA_CFG2, L16_PA_CFG2_MASK, L16_PA_CFG2_RX
    },
    {
        L16_PD_CFG1, L16_DATA_CFG1_MASK, L16_PD_CFG1_RX
    },
    {
        L16_PD_CFG2, L16_PD_CFG2_MASK, L16_PD_CFG2_RX
    },
    {
        L16_PK_CFG0, L16_PK_CFG_MASK, L16_PK_CFG_OUTPUT
    },
};

__attribute__((noinline)) int l16_board_prepare(void)
{
    static const uint32_t timing[] = {
        L16_TIMING_MODE, L16_TIMING_SCALE, L16_TIMING_READY, L16_TIMING_PARITY, L16_TIMING_BYTE_ENABLE,
        L16_TIMING_WAIT, L16_TIMING_CS, L16_TIMING_ALE0, L16_TIMING_ALE1, L16_TIMING_WE,
        L16_TIMING_OE0, L16_TIMING_OE1, L16_TIMING_OE2, L16_TIMING_CYCLE, L16_TIMING_ACCESS,
        L16_TIMING_PAGE, L16_TIMING_DATA_MUX, L16_TIMING_CLE,
        L16_TIMING_SPECIAL, L16_TIMING_SPECIAL, L16_TIMING_SPECIAL,
    };
    uint32_t i, clock;
    if (prepared) return 0;
    clock = l16_read32(L16_CCU_BASE + L16_CCU_LBC_CLOCK);
    if ((clock & L16_CLOCK_SELECT_MASK) != L16_CLOCK_SELECT_EXPECTED ||
        (((clock >> L16_CLOCK_DIV_SHIFT) & L16_CLOCK_DIV_MASK) + 1U) *
        ((clock & L16_CLOCK_DIV_MASK) + 1U) != L16_CLOCK_DIV_TOTAL ||
        (l16_read32(L16_CCU_BASE + L16_CCU_LBC_BGR) & L16_CLOCK_BGR_MASK) != L16_CLOCK_BGR_MASK ||
        (l16_read32(L16_LBC_BASE + L16_IDA_FIFO_STATE) & L16_FIFO_BUSY_MASK)) return L16_BAD_BUS;
    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) saved[i] = l16_read32(L16_GPIO_BASE + pins[i].offset);
    saved_pa_data = l16_read32(L16_GPIO_BASE + L16_PA_DATA);
    saved_pd_data = l16_read32(L16_GPIO_BASE + L16_PD_DATA);
    saved_pk_data = l16_read32(L16_GPIO_BASE + L16_PK_DATA);
    /* 使用置位/清零寄存器，只改变 CS、WR 和译码地址，绝不写 PA/PD 的数据位。 */
    l16_write32(L16_GPIO_BASE + L16_PA_SET, L16_CS_BIT);
    l16_write32(L16_GPIO_BASE + L16_PD_SET, L16_WR_BIT);
    l16_write32(L16_GPIO_BASE + L16_PK_CLEAR, L16_DECODE_LOW);
    l16_write32(L16_GPIO_BASE + L16_PK_SET, L16_DECODE_HIGH);
    prepared = 1;
    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        l16_write32(L16_GPIO_BASE + pins[i].offset,
            (l16_read32(L16_GPIO_BASE + pins[i].offset) & ~pins[i].mask) | pins[i].value);
        if ((l16_read32(L16_GPIO_BASE + pins[i].offset) & pins[i].mask) != pins[i].value) return L16_BAD_BUS;
    }
    /* 沿用已核对的 IDA 接收时序；不映射旧试验中曾挂死的 DA 窗口。 */
    l16_write32(L16_LBC_BASE + L16_INT0_ENABLE, 0);
    l16_write32(L16_LBC_BASE + L16_INT1_ENABLE, 0);
    for (i = 0; i < sizeof(timing) / sizeof(timing[0]); i++)
        l16_write32(L16_LBC_BASE + L16_CS3_TIMING_BASE + i * L16_TIMING_STRIDE, timing[i]);
    l16_write32(L16_LBC_BASE + L16_TURNAROUND, L16_TURNAROUND_VALUE);
    l16_write32(L16_LBC_BASE + L16_CHANNEL_MODE, L16_CHANNEL_IDA);
    l16_write32(L16_LBC_BASE + L16_CS3_DA_HIGH, L16_CS3_HIGH);
    l16_write32(L16_LBC_BASE + L16_CS3_DA_LOW, L16_CS3_LOW);
    l16_write32(L16_LBC_BASE + L16_CS3_IDA_HIGH, L16_CS3_HIGH);
    l16_write32(L16_LBC_BASE + L16_CS3_IDA_LOW, L16_CS3_LOW);
    l16_write32(L16_LBC_BASE + L16_FIXED_CS, L16_FIXED_CS_VALUE);
    l16_write32(L16_LBC_BASE + L16_BUS_CLOCK, L16_BUS_CLOCK_VALUE);
    l16_write32(L16_LBC_BASE + L16_SAMPLE_DELAY, L16_SAMPLE_DELAY_VALUE);
    l16_write32(L16_LBC_BASE + L16_ARBITRATION, L16_ARBITRATION_VALUE);
    l16_write32(L16_LBC_BASE + L16_INT0_PENDING, ~0U);
    l16_write32(L16_LBC_BASE + L16_INT1_PENDING, ~0U);
    l16_write32(L16_LBC_BASE + L16_IDA_ADDRESS, L16_CS3_LOW);
    l16_write32(L16_LBC_BASE + L16_IDA_LENGTH, L16_IDA_ONE_WORD);
    l16_barrier();
    /* 只在初始化时拉低一次 CS；采样、满深度及显示停止期间均不改变 CS。 */
    l16_write32(L16_GPIO_BASE + L16_PA_CLEAR, L16_CS_BIT);
    l16_barrier();
    return 0;
}

int l16_board_read(uint16_t *word, volatile uint32_t *stop)
{
    uint32_t started = (uint32_t)l16_ticks();
    uint32_t state;
    l16_write32(L16_LBC_BASE + L16_INT0_PENDING, L16_IDA_DONE);
    l16_write32(L16_LBC_BASE + L16_IDA_CONTROL, L16_IDA_RX_START);
    /* 单个半字不足一个 32 位 FIFO 字；必须先取尾字，再等 TRSF_DONE。
     * 真实控制器验证过：先等 DONE 会卡在尾字尚未排空的阶段。 */
    while (!((l16_read32(L16_LBC_BASE + L16_IDA_FIFO_STATE) >> L16_FIFO_RX_SHIFT) & L16_FIFO_RX_MASK)) {
        if (*stop || (uint32_t)((uint32_t)l16_ticks() - started) >= L16_READ_TIMEOUT_TICKS) return L16_READ_TIMEOUT;
    }
    *word = (uint16_t)l16_read32(L16_LBC_BASE + L16_IDA_RX_DATA);
    do {
        state = l16_read32(L16_LBC_BASE + L16_INT0_PENDING);
        if (state & L16_INT0_ERRORS) return L16_BUS_ERROR;
        if (state & L16_IDA_DONE) break;
        if (*stop || (uint32_t)((uint32_t)l16_ticks() - started) >= L16_READ_TIMEOUT_TICKS) return L16_READ_TIMEOUT;
    } while (1);
    return 0;
}

int l16_board_check(void)
{
    /* 超时和 FIFO 错误为粘滞位；提交整批原码前必须核验，出错不交付未确认批次。 */
    if ((l16_read32(L16_LBC_BASE + L16_INT0_PENDING) & L16_INT0_ERRORS) ||
        (l16_read32(L16_LBC_BASE + L16_INT1_PENDING) & L16_INT1_ERRORS) ||
        (l16_read32(L16_LBC_BASE + L16_IDA_FIFO_STATE) & L16_FIFO_BUSY_MASK)) return L16_BUS_ERROR;
    return 0;
}

uint16_t l16_board_probe_inputs(void)
{
    uint32_t pa1 = l16_read32(L16_GPIO_BASE + L16_PA_CFG1);
    uint32_t pa2 = l16_read32(L16_GPIO_BASE + L16_PA_CFG2);
    uint32_t pd1 = l16_read32(L16_GPIO_BASE + L16_PD_CFG1);
    uint32_t pd2 = l16_read32(L16_GPIO_BASE + L16_PD_CFG2);
    uint32_t pa, pd, started;
    /* 只在开启监视前取一次输入诊断；CS 不动，数据位始终为输入或 LBC 接收。 */
    l16_write32(L16_GPIO_BASE + L16_PA_CFG1, pa1 & ~L16_DATA_CFG1_MASK);
    l16_write32(L16_GPIO_BASE + L16_PA_CFG2, pa2 & ~L16_DATA_CFG2_MASK);
    l16_write32(L16_GPIO_BASE + L16_PD_CFG1, pd1 & ~L16_DATA_CFG1_MASK);
    l16_write32(L16_GPIO_BASE + L16_PD_CFG2, pd2 & ~L16_DATA_CFG2_MASK);
    l16_barrier();
    started = (uint32_t)l16_ticks();
    while ((uint32_t)((uint32_t)l16_ticks() - started) < L16_PROBE_SETTLE_TICKS) { }
    pa = l16_read32(L16_GPIO_BASE + L16_PA_DATA);
    pd = l16_read32(L16_GPIO_BASE + L16_PD_DATA);
    l16_write32(L16_GPIO_BASE + L16_PA_CFG1, pa1);
    l16_write32(L16_GPIO_BASE + L16_PA_CFG2, pa2);
    l16_write32(L16_GPIO_BASE + L16_PD_CFG1, pd1);
    l16_write32(L16_GPIO_BASE + L16_PD_CFG2, pd2);
    l16_barrier();
    return (uint16_t)(((pd >> L16_DATA_FIRST_PIN) & L16_DATA_BYTE_MASK) |
        (((pa >> L16_DATA_FIRST_PIN) & L16_DATA_BYTE_MASK) << L16_DATA_HIGH_SHIFT));
}

int l16_board_restore(void)
{
    uint32_t i, fifo, started;
    if (!prepared) return 0;
    /* 只有明确关闭裸核时恢复；总线未空闲不能放开资源。 */
    /* 关闭时仅排空已存在的 RX 尾字；不发 START，不把尾字加入正式采样。 */
    started = (uint32_t)l16_ticks();
    do {
        fifo = l16_read32(L16_LBC_BASE + L16_IDA_FIFO_STATE) & L16_FIFO_BUSY_MASK;
        if (!fifo) break;
        if ((fifo >> L16_FIFO_RX_SHIFT) & L16_FIFO_RX_MASK)
            (void)l16_read32(L16_LBC_BASE + L16_IDA_RX_DATA);
        if ((uint32_t)((uint32_t)l16_ticks() - started) >= L16_READ_TIMEOUT_TICKS) return L16_BAD_BUS;
    } while (1);
    l16_write32(L16_GPIO_BASE + L16_PA_SET, L16_CS_BIT);
    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        l16_write32(L16_GPIO_BASE + pins[i].offset,
            (l16_read32(L16_GPIO_BASE + pins[i].offset) & ~pins[i].mask) | (saved[i] & pins[i].mask));
    l16_write32(L16_GPIO_BASE + ((saved_pa_data & L16_CS_BIT) ? L16_PA_SET : L16_PA_CLEAR), L16_CS_BIT);
    l16_write32(L16_GPIO_BASE + ((saved_pd_data & L16_WR_BIT) ? L16_PD_SET : L16_PD_CLEAR), L16_WR_BIT);
    l16_write32(L16_GPIO_BASE + L16_PK_SET, saved_pk_data & L16_DECODE_MASK);
    l16_write32(L16_GPIO_BASE + L16_PK_CLEAR, ~saved_pk_data & L16_DECODE_MASK);
    l16_barrier();
    prepared = 0;
    return 0;
}
