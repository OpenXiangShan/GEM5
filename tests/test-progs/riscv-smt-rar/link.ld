ENTRY(_start)
SECTIONS
{
    . = 0x80000000;
    .text : { *(.text.init) *(.text*) }
    .rodata : { *(.rodata*) }
    . = ALIGN(64);
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
