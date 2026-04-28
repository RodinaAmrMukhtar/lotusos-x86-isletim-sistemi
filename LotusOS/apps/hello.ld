ENTRY(_start)

SECTIONS {
  . = 0x00400000;  /* typical user-space base */

  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(COMMON) *(.bss*) }
}