OUTPUT_ARCH(mips)
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = DEFINED(test_image_base) ? test_image_base : 0x00100000;
    .text : { *(.text .text.*) } :code
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :data
    .data : { *(.data .data.* .sdata .sdata.*) } :data
    _gp = ALIGN(16) + 0x7ff0;
    .bss ALIGN(16) (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    /DISCARD/ : { *(.comment .eh_frame .reginfo .MIPS.abiflags) }
}
