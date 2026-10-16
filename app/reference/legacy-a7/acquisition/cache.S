/* 停止时只清理私有预留区的cache line，禁止全局L2操作。 */
.syntax unified
.cpu cortex-a7
.arm
.text
.global amp_memory_disable
.type amp_memory_disable, %function
amp_memory_disable:
    dsb sy
    ldr r0, =0x44200000
    ldr r1, =0x442e0000
1:  mcr p15, 0, r0, c7, c14, 1
    add r0, r0, #64
    cmp r0, r1
    blo 1b
    dsb sy
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #5
    mcr p15, 0, r0, c1, c0, 0
    isb
    mov r0, #0
    mcr p15, 0, r0, c8, c7, 0
    dsb sy
    isb
    bx lr
.size amp_memory_disable, .-amp_memory_disable
