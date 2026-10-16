/* 停止时只清理私有预留区的cache line，禁止全局L2操作。 */
#include "../logic16-layout.h"
#define SCTLR_DCACHE_MMU 5 /* 仅关闭 SCTLR.C 和 SCTLR.M。 */
#define SCTLR_ICACHE 0x1000 /* 关闭 SCTLR.I。 */
.syntax unified
.cpu cortex-a7
.arm
.text
.global l16_memory_disable
.type l16_memory_disable, %function
l16_memory_disable:
    dsb sy
    ldr r0, =L16_RESERVED_BASE
    ldr r1, =L16_PRIVATE_END
1:  mcr p15, 0, r0, c7, c14, 1
    add r0, r0, #L16_CACHE_LINE_BYTES
    cmp r0, r1
    blo 1b
    dsb sy
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #SCTLR_DCACHE_MMU
    bic r0, r0, #SCTLR_ICACHE
    mcr p15, 0, r0, c1, c0, 0
    isb
    mov r0, #0
    mcr p15, 0, r0, c8, c7, 0
    dsb sy
    isb
    bx lr
.size l16_memory_disable, .-l16_memory_disable
