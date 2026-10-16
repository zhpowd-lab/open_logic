/* 只初始化本核状态；不调用SDK的串口、共享时钟、DDR或ADC初始化。 */
#include "../logic16-layout.h"
#define ARM_MODE_SVC 0x13 /* 启动与主循环使用 SVC 模式。 */
#define ARM_MODE_IRQ 0x12 /* IRQ 使用独立栈。 */
#define ARM_VECTOR_ALIGN 32 /* ARM 向量表的硬件对齐要求。 */
#define ARM_WORD_BYTES 4 /* ARM 指令和通用寄存器宽度。 */
#define PSCI_CALL_LOW 2 /* CPU_OFF 功能号低半字。 */
#define PSCI_CALL_HIGH 0x8400 /* CPU_OFF 功能号高半字。 */
.syntax unified
.cpu cortex-a7
.arm
.section .vectors,"ax"
.balign ARM_VECTOR_ALIGN
.global amp_vectors
amp_vectors:
    b amp_reset
    b amp_trap
    b amp_trap
    b amp_trap
    b amp_trap
    b amp_trap
    b amp_irq_entry
    b amp_trap

.section .text,"ax"
amp_reset:
    cpsid if
    /* PSCI入口须以MMU和D/I cache关闭进入，否则拒绝继续并由启动检查报告。 */
    mrc p15, 0, r0, c1, c0, 0
    ldr r1, =L16_CPU_CACHE_CONTROL
    tst r0, r1
    bne amp_boot_rejected
    cps #ARM_MODE_SVC
    ldr sp, =__svc_stack_top
    cps #ARM_MODE_IRQ
    ldr sp, =__irq_stack_top
    cps #ARM_MODE_SVC
    ldr r0, =amp_vectors
    mcr p15, 0, r0, c12, c0, 0
    isb
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #ARM_WORD_BYTES
    blo 1b
    bl amp_main
    b amp_boot_rejected

amp_irq_entry:
    sub lr, lr, #ARM_WORD_BYTES
    stmdb sp!, {r0-r3, r12, lr}
    bl amp_irq
    ldmia sp!, {r0-r3, r12, pc}^

amp_trap:
    cpsid if
    b amp_boot_rejected

amp_boot_rejected:
    wfi
    b amp_boot_rejected

/* 参数设置与SMC之间禁止调用C函数，避免CPU_OFF被publish的返回寄存器覆盖。 */
.section .text,"ax"
.balign ARM_WORD_BYTES
.global amp_psci_cpu_off
.type amp_psci_cpu_off, %function
amp_psci_cpu_off:
    push {r4, lr}
    movw r0, #PSCI_CALL_LOW
    movt r0, #PSCI_CALL_HIGH
    mov r1, #0
    mov r2, #0
    mov r3, #0
    smc #0
    pop {r4, pc}
.size amp_psci_cpu_off, .-amp_psci_cpu_off
