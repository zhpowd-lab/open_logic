#ifndef OPEN_LOGIC16_LAYOUT_H
#define OPEN_LOGIC16_LAYOUT_H

#if defined(__ASSEMBLER__) || defined(L16_LINKER)
#define L16_LAYOUT_NUMBER(value) value
#else
#define L16_LAYOUT_NUMBER(value) value##U
#endif

/* 共用固定布局；同时供 C、启动汇编和预处理后的链接脚本使用。 */
#define L16_CPU L16_LAYOUT_NUMBER(3) /* CPU3 为独立 A7；Linux 保留 CPU0..2。 */
#define L16_CLOCK L16_LAYOUT_NUMBER(24000000) /* Generic Timer 的实测时钟 Hz。 */
#define L16_MIN_PERIOD L16_LAYOUT_NUMBER(24) /* 最快名义周期：24 tick = 1 微秒。 */
#define L16_MAX_SAMPLES L16_LAYOUT_NUMBER(1000000) /* 一段最多一百万点，不循环覆盖。 */
#define L16_WORD_BYTES L16_LAYOUT_NUMBER(2) /* 原码固定小端 uint16。 */
#define L16_CHANNELS L16_LAYOUT_NUMBER(16) /* bit0..15 对应 D0..15。 */
#define L16_RESERVED_BASE L16_LAYOUT_NUMBER(0x44200000) /* 当前设备树 no-map 预留区起点。 */
#define L16_RESERVED_BYTES L16_LAYOUT_NUMBER(0x200000) /* 当前内核预留两兆字节。 */
#define L16_RESERVED_END (L16_RESERVED_BASE + L16_RESERVED_BYTES) /* 预留区上界。 */
#define L16_PRIVATE_BYTES L16_LAYOUT_NUMBER(0x8000) /* 仅前 32 KiB 为私有缓存区。 */
#define L16_PRIVATE_END (L16_RESERVED_BASE + L16_PRIVATE_BYTES) /* 缓存清理边界。 */
#define L16_L1_TABLE_ADDRESS L16_LAYOUT_NUMBER(0x44208000) /* A7 一级页表，16 KiB 对齐。 */
#define L16_L2_TABLE_ADDRESS L16_LAYOUT_NUMBER(0x4420c000) /* 首个 MiB 的二级页表。 */
#define L16_STATUS_ADDRESS L16_LAYOUT_NUMBER(0x4420f000) /* 不缓存的共享状态页。 */
#define L16_PAGE_BYTES L16_LAYOUT_NUMBER(0x1000) /* ARM 小页与 Linux mmap 对齐大小。 */
#define L16_STATUS_BYTES L16_LAYOUT_NUMBER(144) /* ABI3 固定 36 个 uint32 字段。 */
#define L16_STATUS_WORDS L16_LAYOUT_NUMBER(36) /* 状态字段总数。 */
#define L16_SAMPLE_ADDRESS L16_LAYOUT_NUMBER(0x44210000) /* 原始连续样本区起点。 */
#define L16_SAMPLE_BYTES (L16_MAX_SAMPLES * L16_WORD_BYTES) /* 最大原码深度。 */
#define L16_IRQ_STACK_BYTES L16_LAYOUT_NUMBER(0x800) /* 私有 IRQ 栈。 */
#define L16_SVC_STACK_BYTES L16_LAYOUT_NUMBER(0x2000) /* 私有主栈。 */
#define L16_CPU_CACHE_CONTROL L16_LAYOUT_NUMBER(0x1005) /* SCTLR 的 I、C、M 位。 */
#define L16_CACHE_LINE_BYTES L16_LAYOUT_NUMBER(64) /* Cortex-A7 的缓存行大小。 */
#define L16_PSCI_CPU_OFF L16_LAYOUT_NUMBER(0x84000002) /* PSCI 32 位 CPU_OFF 功能号。 */

#endif
