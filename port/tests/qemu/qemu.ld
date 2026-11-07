/* Static ARM Linux layout for the emulated harness. */
ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text : { *(.text._start) *(.text .text.*) *(.gnu.linkonce.t.*) }
    .init : { KEEP(*(.init)) }
    .fini : { KEEP(*(.fini)) }
    .rodata : { *(.rodata .rodata.*) }
    .ARM.extab : { *(.ARM.extab*) }
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; }
    . = ALIGN(0x1000);
    .data : {
        *(.data .data.*)
        . = ALIGN(4);
        __preinit_array_start = .; KEEP(*(.preinit_array)) __preinit_array_end = .;
        __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .;
        __fini_array_start = .; KEEP(*(.fini_array*)) __fini_array_end = .;
    }
    .tdata : { __tdata_start = .; *(.tdata .tdata.*) __tdata_end = .; }
    .tbss : { __tbss_start = .; *(.tbss .tbss.*) *(.tcommon) __tbss_end = .; }
    . = ALIGN(16);
    __bss_start__ = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    __bss_end__ = .;
    _end = .;
    __end__ = .;
    /DISCARD/ : { *(.comment) *(.note*) }
}
