/* 代码/栈、诊断页与原RAW80共享环在同一2MiB no-map内，互不重叠。 */
ENTRY(amp_vectors)
MEMORY {
    ram (rwx) : ORIGIN = 0x44200000, LENGTH = 0x000f0000
    status (rw) : ORIGIN = 0x442f0000, LENGTH = 0x00001000
}
SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > ram
    .text : { *(.text*) } > ram
    .rodata : { *(.rodata*) } > ram
    .data : { *(.data*) } > ram
    .bss (NOLOAD) : {
        . = ALIGN(4); __bss_start = .; *(.bss*) *(COMMON)
        . = ALIGN(4); __bss_end = .;
    } > ram
    .stack (NOLOAD) : {
        . = ALIGN(16); . += 0x4000; __irq_stack_top = .;
        . += 0x8000; __svc_stack_top = .;
    } > ram
    .translation_tables 0x442e0000 (NOLOAD) : { . += 0x5000; } > ram
    .acq_status : { KEEP(*(.acq_status)) } > status
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
    ASSERT(__svc_stack_top <= 0x442e0000, "AMP code/stack overflow")
    ASSERT(SIZEOF(.acq_status) == 128, "AMP status layout differs")
}
