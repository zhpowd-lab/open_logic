#ifndef OPEN_LOGIC16_BOARD_REGISTERS_H
#define OPEN_LOGIC16_BOARD_REGISTERS_H

/* 依据当前 T153 SDK LBC v2 寄存器定义；时序保持已验证的 100 MHz 配置。 */
enum {
    L16_GPIO_BASE = 0x03604000U, /* 主域 PIO，禁止整字写 PA/PD 数据寄存器。 */
    L16_LBC_BASE = 0x02810000U, /* LBC v2 仅配置接收事务。 */
    L16_LBC_MAP_BYTES = 0x1000U, /* Linux 独占的寄存器范围。 */
    L16_CCU_BASE = 0x02002000U, /* 只读核对，时钟写入归 Linux 时钟框架。 */
    L16_PA_CFG1 = 0x84U, /* PA12..15 的 mux 字段。 */
    L16_PA_CFG2 = 0x88U, /* PA16..19 数据与 PA21 CS 的 mux 字段。 */
    L16_PA_DATA = 0x90U, /* PA 原始输出锁存，用于恢复 CS 位。 */
    L16_PA_SET = 0x94U, /* PA 原子置位。 */
    L16_PA_CLEAR = 0x98U, /* PA 原子清零。 */
    L16_PD_CFG1 = 0x204U, /* PD12..15 的 mux 字段。 */
    L16_PD_CFG2 = 0x208U, /* PD16..19 数据、PD20 WR、PD21 RD。 */
    L16_PD_DATA = 0x210U, /* PD 原始输出锁存，仅保存 WR 位。 */
    L16_PD_SET = 0x214U, /* PD 原子置位。 */
    L16_PD_CLEAR = 0x218U, /* PD 原子清零。 */
    L16_PK_CFG0 = 0x580U, /* PK3..5 为外部译码器地址。 */
    L16_PK_DATA = 0x590U, /* PK 输出锁存的原值。 */
    L16_PK_SET = 0x594U, /* PK 原子置位。 */
    L16_PK_CLEAR = 0x598U, /* PK 原子清零。 */
    L16_CS_BIT = 0x00200000U, /* PA21：整个采样段保持低。 */
    L16_WR_BIT = 0x00100000U, /* PD20：整个采样段保持高，绝不产生 WR。 */
    L16_DECODE_MASK = 0x38U, /* PK3..5 的掩码。 */
    L16_DECODE_HIGH = 0x28U, /* 101，选择原理图 U15/U16 对应的 Y5。 */
    L16_DECODE_LOW = 0x10U, /* 译码地址中须保持低的 PK4。 */
    L16_DATA_CFG1_MASK = 0xffff0000U, /* 一个端口的前四根数据 mux。 */
    L16_DATA_CFG2_MASK = 0x0000ffffU, /* 仅后四根数据，不改变 RD、WR、CS。 */
    L16_DATA_BYTE_MASK = 0xffU,
    L16_DATA_FIRST_PIN = 12U,
    L16_DATA_HIGH_SHIFT = 8U,
    L16_PROBE_SETTLE_TICKS = 24U, /* GPIO 输入诊断仅等待一微秒，不发总线事务。 */
    L16_PA_CFG1_RX = 0x66660000U, /* PA12..15=LBC mux6。 */
    L16_PD_CFG1_RX = 0x88880000U, /* PD12..15=LBC mux8。 */
    L16_PA_CFG2_MASK = 0x00f0ffffU, /* 不触碰 PA20 及其它无关引脚。 */
    L16_PA_CFG2_RX = 0x00106666U, /* 数据 mux6，CS 为 GPIO 输出。 */
    L16_PD_CFG2_MASK = 0x00ffffffU, /* PD16..21 所有权范围。 */
    L16_PD_CFG2_RX = 0x00818888U, /* 数据/RD mux8，WR 为固定 GPIO 输出。 */
    L16_PK_CFG_MASK = 0x00fff000U, /* PK3..5 的 mux 范围。 */
    L16_PK_CFG_OUTPUT = 0x00111000U, /* 只把译码地址设为输出。 */
    L16_CCU_LBC_CLOCK = 0x1040U, /* LBC 时钟源和分频。 */
    L16_CCU_LBC_BGR = 0x104cU, /* LBC 门控和复位释放状态。 */
    L16_CLOCK_SELECT_MASK = 0x87000000U, /* 门控与时钟源字段。 */
    L16_CLOCK_SELECT_EXPECTED = 0x81000000U, /* 已核对的 400 MHz 父时钟。 */
    L16_CLOCK_DIV_SHIFT = 8U, /* 二级分频字段位置。 */
    L16_CLOCK_DIV_MASK = 31U, /* 分频字段为除数减一。 */
    L16_CLOCK_DIV_TOTAL = 4U, /* 400 MHz / 4 = 100 MHz。 */
    L16_CLOCK_BGR_MASK = 0x10001U, /* 门控开启且复位已释放。 */
    L16_LBC_PARENT_HZ = 400000000U, /* 驱动核对的父时钟。 */
    L16_LBC_CLOCK_HZ = 100000000U, /* 本项目 LBC 时序的计算基准。 */
    L16_CS3_TIMING_BASE = 0x180U, /* 内部 CS3 的时序寄存器组。 */
    L16_TIMING_STRIDE = 4U, /* 每个时序寄存器为一个 uint32。 */
    L16_TIMING_MODE = 0x02011000U, /* 16 位异步读，地址不复用。 */
    L16_TIMING_SCALE = 0U, /* 保持原始时钟刻度。 */
    L16_TIMING_READY = 0x00c10021U, /* 沿用已核对的 READY 等待设置。 */
    L16_TIMING_PARITY = 0x101U, /* 原 SDK 数据校验配置。 */
    L16_TIMING_BYTE_ENABLE = 0x101U, /* 原 SDK 字节使能配置。 */
    L16_TIMING_WAIT = 0x04000100U, /* WAIT 及其超时设置。 */
    L16_TIMING_CS = 0x09090000U, /* 内部 CS 的起止时序，外部 CS 由 GPIO 常开。 */
    L16_TIMING_ALE0 = 1U, /* ALE 基础配置。 */
    L16_TIMING_ALE1 = 0U, /* 不使用地址复用。 */
    L16_TIMING_WE = 0x00060200U, /* 保留控制器配置；物理 WR 固定高。 */
    L16_TIMING_OE0 = 0x00070200U, /* RD 的有效区间。 */
    L16_TIMING_OE1 = 0U, /* 无地址复用读时序。 */
    L16_TIMING_OE2 = 1U, /* 原控制器页时序配置。 */
    L16_TIMING_CYCLE = 0x000c000aU, /* 原有读写周期设置。 */
    L16_TIMING_ACCESS = 0x00060004U, /* 原有读写取数时刻。 */
    L16_TIMING_PAGE = 0U, /* 不使用页访问。 */
    L16_TIMING_DATA_MUX = 7U, /* 原控制器数据时序。 */
    L16_TIMING_CLE = 0x11U, /* 原控制器 CLE 配置。 */
    L16_TIMING_SPECIAL = 0U, /* 不启用 NAND/LCD 等特殊模式。 */
    L16_TURNAROUND = 0x1e0U, /* CS3 总线周转寄存器。 */
    L16_TURNAROUND_VALUE = 2U, /* 保持原有周转周期。 */
    L16_CHANNEL_MODE = 0x200U, /* 只使用 IDA 的通道选择。 */
    L16_CHANNEL_IDA = 1U, /* IDA 模式。 */
    L16_CS3_DA_HIGH = 0x240U, /* CS3 DA 地址上界。 */
    L16_CS3_DA_LOW = 0x244U, /* CS3 DA 地址下界；不映射该窗口。 */
    L16_CS3_IDA_HIGH = 0x248U, /* CS3 IDA 地址上界。 */
    L16_CS3_IDA_LOW = 0x24cU, /* CS3 IDA 地址下界。 */
    L16_CS3_HIGH = 0x3fffffffU, /* CS3 内部地址范围终点。 */
    L16_CS3_LOW = 0x30000000U, /* 本项目固定接收地址。 */
    L16_FIXED_CS = 0x250U, /* 内部固定片选配置。 */
    L16_FIXED_CS_VALUE = 0x10000U, /* 保持旧配置。 */
    L16_BUS_CLOCK = 0x280U, /* LBC 本地时钟配置。 */
    L16_BUS_CLOCK_VALUE = 0x10005U, /* 原有采样边沿与门控配置。 */
    L16_SAMPLE_DELAY = 0x290U, /* LBC 接收采样延迟。 */
    L16_SAMPLE_DELAY_VALUE = 0x10000U, /* 沿用已验证的旁路设置。 */
    L16_IDA_CONTROL = 0x300U, /* START 及读写方向。 */
    L16_IDA_RX_START = 0x00038001U, /* RX 单字事务；方向位为读。 */
    L16_IDA_ADDRESS = 0x304U, /* 固定内部 CS3 地址。 */
    L16_IDA_LENGTH = 0x308U, /* 以半字表示的接收长度。 */
    L16_IDA_ONE_WORD = 1U, /* 每笔只读一个 16 位字。 */
    L16_IDA_RX_DATA = 0x350U, /* 只读 FIFO；禁止访问 TX 0x340。 */
    L16_ARBITRATION = 0x440U, /* 控制器仲裁优先级。 */
    L16_ARBITRATION_VALUE = 0x111U, /* 保持原有仲裁设置。 */
    L16_INT0_ENABLE = 0x500U, /* 不向 Linux/CPU3 注册 LBC 中断。 */
    L16_INT1_ENABLE = 0x504U, /* 关闭第二组 LBC 中断。 */
    L16_INT0_PENDING = 0x508U, /* DONE、READY、FIFO 等粘滞标志。 */
    L16_INT1_PENDING = 0x50cU, /* IDA 及数据请求超时标志。 */
    L16_IDA_FIFO_STATE = 0x518U, /* RX/TX/命令水位。 */
    L16_IDA_DONE = 8U, /* IDA TRSF_DONE，写一清零。 */
    L16_INT0_ERRORS = 0x0ffff0f2U, /* READY、FIFO、校验及异常通道标志。 */
    L16_INT1_ERRORS = 0x74U, /* 控制器超时和数据请求超时。 */
    L16_FIFO_BUSY_MASK = 0x01ff1ff7U, /* 三类 FIFO 非空字段。 */
    L16_FIFO_RX_SHIFT = 16U, /* RX FIFO 水位位置。 */
    L16_FIFO_RX_MASK = 0x1ffU, /* RX FIFO 水位宽度。 */
    L16_FIFO_NON_RX_MASK = 0x1ff7U, /* TX 与命令水位。 */
    L16_GPIO_COUNT = 22U, /* 16 数据、RD/WR、CS 与三根译码线。 */
    L16_PIN_CONFIG_COUNT = 5U, /* PA、PD 和 PK 共五个 mux 寄存器。 */
    L16_DT_GPIO_COUNT = 25U, /* 当前旧资源节点共有 25 个 GPIO。 */
    L16_DT_GPIO_FIRST = 2U, /* 跳过 ADC RESET/BUSY 两个无关 GPIO。 */
};

#endif
