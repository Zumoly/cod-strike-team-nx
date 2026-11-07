ENTRY(_start)
PHDRS
{
    code PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0;
    .text : { *(.text.entry) . = ALIGN(0x1000); *(.text.target) . = ALIGN(0x1000); } :code
    .rodata : { *(.rodata) *(.note*) . = ALIGN(0x1000); } :rodata
    .data : { *(.data) . = ALIGN(0x1000); } :data
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
