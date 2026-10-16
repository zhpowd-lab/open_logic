#include "platform.h"
enum {
    MMU_L1_ENTRIES = 4096U, /* ARM 短描述符一级表项数。 */
    MMU_L2_ENTRIES = 256U, /* 一个 MiB 的小页表项数。 */
    MMU_SECTION_SHIFT = 20U, /* 一级节映射为一 MiB。 */
    MMU_PAGE_SHIFT = 12U, /* 二级小页映射为四 KiB。 */
    MMU_DEVICE_BEGIN = 0x20U, /* 设备寄存器首个 MiB。 */
    MMU_DEVICE_END = 0x50U, /* 设备寄存器最后一个 MiB，不包含。 */
    MMU_PRIVATE_SECTION = 0x442U, /* 前一 MiB 使用小页区分私有区和状态区。 */
    MMU_SAMPLE_SECTION = 0x443U, /* 后一 MiB 全部为不缓存样本。 */
    MMU_DEVICE_FLAGS = 0x10c16U, /* 共享 Device、读写、不可执行。 */
    MMU_NORMAL_NC_FLAGS = 0x11c12U, /* 共享 Normal NC 节、读写、不可执行。 */
    MMU_PRIVATE_PAGE_FLAGS = 0x7eU, /* 私有 WBWA 小页，代码可执行。 */
    MMU_STATUS_PAGE_FLAGS = 0x437U, /* 页表和共享状态为 Device，不缓存。 */
    MMU_SAMPLE_PAGE_FLAGS = 0x473U, /* 样本为共享 Normal NC，不可执行。 */
    MMU_COARSE_TABLE_TYPE = 1U, /* 指向二级粗页表。 */
    MMU_DOMAIN_CLIENT = 1U, /* Domain 0 使用表项权限。 */
    CACHE_SET_SHIFT = 13U, /* CCSIDR 的 sets 字段。 */
    CACHE_SET_MASK = 0x7fffU, /* sets 字段掩码。 */
    CACHE_WAY_SHIFT = 3U, /* CCSIDR 的 ways 字段。 */
    CACHE_WAY_MASK = 0x3ffU, /* ways 字段掩码。 */
    CACHE_LINE_MASK = 7U, /* CCSIDR 行长度字段。 */
    CACHE_LINE_LOG_BIAS = 4U, /* 行长度字段到字节对数的偏移。 */
    SCTLR_CLEAR_MASK = 0x30082000U, /* 关闭 TRE/AFE、旧中断模型与高向量。 */
};

void l16_memory_enable(void)
{
    volatile uint32_t *sections = (volatile uint32_t *)L16_L1_TABLE_ADDRESS;
    volatile uint32_t *pages = (volatile uint32_t *)L16_L2_TABLE_ADDRESS;
    uint32_t i, zero = 0, cache, sets, ways, line_shift, way_shift, control;
    for (i = 0; i < MMU_L1_ENTRIES; i++) sections[i] = 0;
    for (i = MMU_DEVICE_BEGIN; i < MMU_DEVICE_END; i++) sections[i] = (i << MMU_SECTION_SHIFT) | MMU_DEVICE_FLAGS;
    sections[MMU_PRIVATE_SECTION] = L16_L2_TABLE_ADDRESS | MMU_COARSE_TABLE_TYPE;
    sections[MMU_SAMPLE_SECTION] = (MMU_SAMPLE_SECTION << MMU_SECTION_SHIFT) | MMU_NORMAL_NC_FLAGS;
    for (i = 0; i < MMU_L2_ENTRIES; i++) {
        uint32_t flags = i < L16_PRIVATE_BYTES / L16_PAGE_BYTES ? MMU_PRIVATE_PAGE_FLAGS :
            (i < (L16_SAMPLE_ADDRESS - L16_RESERVED_BASE) / L16_PAGE_BYTES ? MMU_STATUS_PAGE_FLAGS : MMU_SAMPLE_PAGE_FLAGS);
        pages[i] = (L16_RESERVED_BASE + (i << MMU_PAGE_SHIFT)) | flags;
    }
    /* 只失效 CPU3 L1，不对 Linux 共用的 L2 进行 set/way 清理。 */
    __asm__ volatile("mcr p15, 2, %0, c0, c0, 0\nisb\nmrc p15, 1, %1, c0, c0, 0" : "+r"(zero), "=r"(cache) :: "memory");
    sets = (cache >> CACHE_SET_SHIFT) & CACHE_SET_MASK;
    ways = (cache >> CACHE_WAY_SHIFT) & CACHE_WAY_MASK;
    line_shift = (cache & CACHE_LINE_MASK) + CACHE_LINE_LOG_BIAS;
    way_shift = ways ? (uint32_t)__builtin_clz(ways) : 0;
    for (i = 0; i <= sets; i++) {
        uint32_t way;
        for (way = 0; way <= ways; way++) {
            uint32_t operand = (i << line_shift) | (way << way_shift);
            __asm__ volatile("mcr p15, 0, %0, c7, c6, 2" :: "r"(operand) : "memory");
        }
    }
    __asm__ volatile("dsb sy\nmcr p15, 0, %0, c7, c5, 0\nmcr p15, 0, %0, c8, c7, 0\nmcr p15, 0, %0, c2, c0, 2" :: "r"(zero) : "memory");
    i = L16_L1_TABLE_ADDRESS;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(i) : "memory");
    i = MMU_DOMAIN_CLIENT;
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmrc p15, 0, %1, c1, c0, 0" : "+r"(i), "=r"(control) :: "memory");
    control &= ~SCTLR_CLEAR_MASK;
    control |= L16_CPU_CACHE_CONTROL;
    __asm__ volatile("dsb sy\nmcr p15, 0, %0, c1, c0, 0\nisb" :: "r"(control) : "memory");
}
