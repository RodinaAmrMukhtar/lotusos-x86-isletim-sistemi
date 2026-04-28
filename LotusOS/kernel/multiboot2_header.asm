bits 32

; Multiboot2 header (for Limine's multiboot2 protocol or any MB2-capable bootloader)
; Must be within first 32 KiB of the kernel image and 8-byte aligned.

section .multiboot2
align 8

MB2_MAGIC        equ 0xE85250D6
MB2_ARCH_I386    equ 0
MB2_HEADER_START:

dd MB2_MAGIC
dd MB2_ARCH_I386
dd (MB2_HEADER_END - MB2_HEADER_START)
dd -(MB2_MAGIC + MB2_ARCH_I386 + (MB2_HEADER_END - MB2_HEADER_START))

; ---- Framebuffer request tag ----
; type = 5, size = 24
; width/height/depth can be 0 to accept any, but we request 1024x768x32 and let bootloader fall back if unavailable.
align 8
dw 5              ; tag type
dw 0              ; flags
dd 24             ; size
dd 1024           ; width
dd 768            ; height
dd 32             ; depth

; ---- End tag ----
align 8
dw 0
dw 0
dd 8

MB2_HEADER_END:
