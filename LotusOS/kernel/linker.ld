ENTRY(_start)

SECTIONS
{
  . = 0x00100000;

  .text : {
    KEEP(*(.multiboot2))
    *(.text.start)
    *(.text*)
  }

  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(COMMON) *(.bss*) }

  /* End-of-kernel symbol for memory manager */
  . = ALIGN(4096);
  __kernel_end = .;

  /DISCARD/ : {
    *(.comment*)
    *(.note*)
    *(.eh_frame*)
    *(.eh_frame_hdr*)
  }
}