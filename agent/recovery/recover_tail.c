// SPDX-License-Identifier: GPL-2.0
/* 仅修复本次 DONE 次序试验遗留的一个 RX 尾字；不发新事务、不写 GPIO。 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/delay.h>

#define STATUS_BASE 0x4420f000U /* 本项目共享状态页。 */
#define LBC_BASE 0x02810000U /* 本项目独占的 LBC。 */
#define MAP_BYTES 0x1000U /* 只映射一页寄存器。 */
#define STATUS_MAGIC 0x3631474cU /* L16 ABI 魔数。 */
#define MAGIC_OFFSET 4U /* 状态魔数偏移。 */
#define STATE_OFFSET 20U /* 采集状态偏移。 */
#define ERROR_OFFSET 44U /* 错误字段偏移。 */
#define FAULT_STATE 5U /* 只允许已停止的故障段。 */
#define TIMEOUT_ERROR 4U /* 只允许本次 RX 超时。 */
#define SAMPLE_CPU 3U /* CPU3 必须仍由本项目持有。 */
#define FIFO_STATE 0x518U /* RX/TX/命令 FIFO 水位。 */
#define RX_DATA 0x350U /* 只读 RX 数据寄存器。 */
#define INT_STATE 0x508U /* 用于记录 DONE 和粘滞错误。 */
#define FIFO_MASK 0x01ff1ff7U /* 所有非空水位。 */
#define SINGLE_TAIL 0x00010000U /* 只允许恰好一个 RX 字、TX/命令均为空。 */
#define MAX_CHECKS 100U /* 最多观察 100 微秒后返回。 */
#define CHECK_US 1U /* 观察间隔一微秒。 */

static int __init recover(void)
{
    void __iomem *status = ioremap(STATUS_BASE, MAP_BYTES);
    void __iomem *bus = ioremap(LBC_BASE, MAP_BYTES);
    u32 fifo, flags;
    unsigned int retry;
    int result = -EBUSY;
    if (!status || !bus) { result = -ENOMEM; goto out; }
    if (cpu_online(SAMPLE_CPU) || readl(status + MAGIC_OFFSET) != STATUS_MAGIC ||
        readl(status + STATE_OFFSET) != FAULT_STATE || readl(status + ERROR_OFFSET) != TIMEOUT_ERROR) goto out;
    fifo = readl(bus + FIFO_STATE) & FIFO_MASK;
    flags = readl(bus + INT_STATE);
    pr_info("logic16 recovery: before fifo=%08x flags=%08x\n", fifo, flags);
    if (fifo != SINGLE_TAIL) goto out;
    readl(bus + RX_DATA);
    for (retry = 0; retry < MAX_CHECKS; retry++) {
        if (!(readl(bus + FIFO_STATE) & FIFO_MASK)) { result = 0; break; }
        udelay(CHECK_US);
    }
    pr_info("logic16 recovery: after fifo=%08x flags=%08x result=%d\n",
            readl(bus + FIFO_STATE), readl(bus + INT_STATE), result);
out:
    if (status) iounmap(status);
    if (bus) iounmap(bus);
    return result;
}
static void __exit finish(void) { }
module_init(recover);
module_exit(finish);
MODULE_LICENSE("GPL");
