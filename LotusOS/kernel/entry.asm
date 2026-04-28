bits 32

section .text.start
global _start
extern kentry

_start:
  cli
  mov esp, 0x009FC00

  ; Multiboot2: EAX = bootloader magic, EBX = pointer to multiboot2 info struct (physical)
  push ebx          ; 2nd arg: info ptr
  push eax          ; 1st arg: magic
  call kentry
  add esp, 8

.hang:
  hlt
  jmp .hang
