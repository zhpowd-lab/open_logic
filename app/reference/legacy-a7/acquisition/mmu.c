#include "rt_shared_abi.h"
#include "rt_acquisition_target.h"

/* SDK ARMv7短描述符布局；私有代码/栈可缓存，环为Normal NC，状态/MMIO为Device。 */
#define TABLE_ADDRESS 0x442e0000U
#define SMALL_ADDRESS 0x442e4000U
#define DEVICE_SECTION ((1U << 16) | (3U << 10) | (1U << 4) | 4U | 2U)
#define PRIVATE_PAGE ((1U << 6) | (3U << 4) | 12U | 2U)
#define DEVICE_PAGE ((1U << 10) | (3U << 4) | 4U | 3U)
#define SHARED_NC_SECTION ((1U << 16) | (1U << 12) | (3U << 10) | (1U << 4) | 2U)

void amp_memory_enable(void)
{
    volatile rt_u32 *sections = (volatile rt_u32 *)TABLE_ADDRESS;
    volatile rt_u32 *pages = (volatile rt_u32 *)SMALL_ADDRESS;
    rt_u32 i, control, zero = 0, cache, sets, ways, line_shift, way_shift;
    for (i = 0; i < 4096; i++) sections[i] = 0;
    for (i = 0x20; i < 0x50; i++) sections[i] = (i << 20) | DEVICE_SECTION;
    /* 与Linux ioremap_wc/pgprot_writecombine一致：共享DDR为Normal NC，
     * 允许总线合并写，仍无D-cache副本；发布/消费游标继续使用显式屏障。 */
    sections[0x443] = 0x44300000U | SHARED_NC_SECTION;
#if ADC_LBC_MODE == ADC_LBC_DA
    /* 仅映射SDK内部CS3窗口首1MiB；Device且XN，不允许投机取样。 */
    sections[0xf00] = 0xf0000000U | DEVICE_SECTION;
#endif
    sections[0x442] = SMALL_ADDRESS | 1U;
    for (i = 0; i < 256; i++)
        pages[i] = (0x44200000U + (i << 12)) | (i < 0xe0 ? PRIVATE_PAGE : DEVICE_PAGE);
    /* 只失效CPU3 L1，禁止SDK通用全层set/way循环触及Linux共享L2。 */
    __asm__ volatile("mcr p15, 2, %0, c0, c0, 0\nisb\nmrc p15, 1, %1, c0, c0, 0"
                     : "+r"(zero), "=r"(cache) :: "memory");
    sets = (cache >> 13) & 0x7fffU;
    ways = (cache >> 3) & 0x3ffU;
    line_shift = (cache & 7U) + 4U;
    way_shift = ways ? (rt_u32)__builtin_clz(ways) : 0;
    for (i = 0; i <= sets; i++) {
        rt_u32 way;
        for (way = 0; way <= ways; way++) {
            rt_u32 operand = (i << line_shift) | (way << way_shift);
            __asm__ volatile("mcr p15, 0, %0, c7, c6, 2" :: "r"(operand) : "memory");
        }
    }
    __asm__ volatile("dsb sy\nmcr p15, 0, %0, c8, c7, 0\nmcr p15, 0, %0, c2, c0, 2"
                     :: "r"(zero) : "memory");
    i = TABLE_ADDRESS;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(i) : "memory");
    i = 1;
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmrc p15, 0, %1, c1, c0, 0"
                     : "+r"(i), "=r"(control) :: "memory");
    control &= ~((1U << 28) | (1U << 29) | (1U << 19) | (1U << 13));
    control |= 5U;
    __asm__ volatile("dsb sy\nmcr p15, 0, %0, c1, c0, 0\nisb" :: "r"(control) : "memory");
}
