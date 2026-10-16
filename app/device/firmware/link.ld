#include "../logic16-layout.h"
#define LINK_WORD_ALIGN 4 /* uint32 对齐。 */
#define LINK_STACK_ALIGN 16 /* ARM ABI 栈对齐。 */
ENTRY(amp_vectors)
MEMORY {
    ram (rwx) : ORIGIN = L16_RESERVED_BASE, LENGTH = L16_PRIVATE_BYTES
    status (rw) : ORIGIN = L16_STATUS_ADDRESS, LENGTH = L16_PAGE_BYTES
}
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > ram
    .text : { *(.text*) } > ram
    .rodata : { *(.rodata*) } > ram
    .resource_table : { KEEP(*(.resource_table)) } > ram
    .version_table : { KEEP(*(.version_table)) } > ram
    .data : { *(.data*) } > ram
    .bss (NOLOAD) : {
        . = ALIGN(LINK_WORD_ALIGN); __bss_start = .; *(.bss*) *(COMMON)
        . = ALIGN(LINK_WORD_ALIGN); __bss_end = .;
    } > ram
    .stack (NOLOAD) : {
        . = ALIGN(LINK_STACK_ALIGN); . += L16_IRQ_STACK_BYTES; __irq_stack_top = .;
        . += L16_SVC_STACK_BYTES; __svc_stack_top = .;
    } > ram
    .status : { KEEP(*(.status)) } > status
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
    ASSERT(__svc_stack_top <= L16_PRIVATE_END, "CPU3 private RAM overflow")
    ASSERT(SIZEOF(.status) == L16_STATUS_BYTES, "logic16 status layout changed")
}
