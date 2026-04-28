; stage2.asm - second stage loader (loaded at 0000:8000)
; A20, E820, load kernel to 0x10000, optional VBE LFB, copy BIOS font to 0x6000,
; enter protected mode, copy kernel to 1MB, jump.
;
; Boot info written to physical 0x00005000 and passed to kernel in EAX.

bits 16
org 0x8000

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 32
%endif

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 64
%endif

%define KERNEL_LBA     (1 + STAGE2_SECTORS)
%define KERNEL_SRC_SEG 0x1000
%define KERNEL_DWORDS  (KERNEL_SECTORS*512/4)

; boot info layout
%define BOOTINFO_ADDR 0x00005000
%define E820_ADDR     0x00005100
%define E820_MAX      64
%define FONT_ADDR     0x00006000

; boot_info_t offsets
%define BI_MAGIC        0
%define BI_E820_COUNT   4
%define BI_E820_ADDR    8
%define BI_BOOT_DRIVE   12
%define BI_FB_ADDR      16
%define BI_FB_PITCH     20
%define BI_FB_W         24
%define BI_FB_H         28
%define BI_FB_BPP       32
%define BI_FB_TYPE      36
%define BI_FONT_ADDR    40
%define BI_FONT_H       44

start2:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7C00
  sti

  mov [boot_drive], dl

  mov si, msg_stage2
  call puts

  call enable_a20
  call a20_check
  cmp ax, 1
  jne a20_fail

  mov si, msg_a20_ok
  call puts

  call bootinfo_init
  call e820_collect

  cmp dword [BOOTINFO_ADDR + BI_E820_COUNT], 0
  jne .e820_ok
  mov si, msg_e820_fail
  call puts
  jmp .after_e820
.e820_ok:
  mov si, msg_e820_ok
  call puts
.after_e820:

  mov si, msg_font
  call puts
  call font_copy
  jc .font_fail
  mov si, msg_font_ok
  call puts
  jmp .after_font
.font_fail:
  mov si, msg_font_fail
  call puts
.after_font:

  mov si, msg_loadk
  call puts

  call read_kernel_edd
  jc read_fail

  mov si, msg_read_ok
  call puts

  mov si, msg_video_try
  call puts
  call vbe_init
  jc .vbe_fail
  mov si, msg_video_ok
  call puts
  jmp .after_vbe
.vbe_fail:
  mov si, msg_video_fail
  call puts
.after_vbe:

  xor ax, ax
  mov ds, ax
  mov es, ax

  mov si, msg_pm
  call puts

  cli
  lgdt [gdt_desc]

  mov eax, cr0
  or eax, 1
  mov cr0, eax
  jmp 0x08:pm_entry

; ------------------------------------------------------------
; Read kernel with INT 13h Extensions in chunks
; ------------------------------------------------------------

read_kernel_edd:
  mov ax, KERNEL_SRC_SEG
  mov es, ax
  xor bx, bx

  mov byte [int13_err], 0

  mov dword [dap + 8], KERNEL_LBA
  mov dword [dap + 12], 0

  mov cx, KERNEL_SECTORS

.rk_loop:
  or cx, cx
  jz .rk_ok

  mov ax, cx
  cmp ax, 32
  jbe .cnt_ok
  mov ax, 32
.cnt_ok:

  mov word [dap + 2], ax
  mov word [dap + 4], bx
  mov word [dap + 6], es

  mov dl, [boot_drive]
  mov ah, 0x42
  mov si, dap
  int 0x13
  jc .rk_fail

  mov dx, ax
  shl dx, 5

  push ax
  mov ax, es
  add ax, dx
  mov es, ax
  pop ax

  xor edx, edx
  mov dx, ax
  add dword [dap + 8], edx

  sub cx, ax
  jmp .rk_loop

.rk_ok:
  clc
  ret

.rk_fail:
  mov [int13_err], ah
  stc
  ret

read_fail:
  mov si, msg_read_fail
  call puts
  mov al, [int13_err]
  call print_hex8
  mov si, msg_nl
  call puts
  jmp $

a20_fail:
  mov si, msg_a20_fail
  call puts
  jmp $

; ------------------------------------------------------------
; Protected mode
; ------------------------------------------------------------

bits 32
pm_entry:
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  mov esp, 0x009FC00

  cld
  mov esi, 0x00010000
  mov edi, 0x00100000
  mov ecx, KERNEL_DWORDS
  rep movsd

  mov eax, BOOTINFO_ADDR
  jmp 0x00100000

bits 16

; ------------------------------------------------------------
; Boot info / E820 / font
; ------------------------------------------------------------

bootinfo_init:
  mov dword [BOOTINFO_ADDR + BI_MAGIC], 0x544F4F42
  mov dword [BOOTINFO_ADDR + BI_E820_COUNT], 0
  mov dword [BOOTINFO_ADDR + BI_E820_ADDR], E820_ADDR

  xor eax, eax
  mov al, [boot_drive]
  mov dword [BOOTINFO_ADDR + BI_BOOT_DRIVE], eax

  mov dword [BOOTINFO_ADDR + BI_FB_ADDR], 0
  mov dword [BOOTINFO_ADDR + BI_FB_PITCH], 0
  mov dword [BOOTINFO_ADDR + BI_FB_W], 0
  mov dword [BOOTINFO_ADDR + BI_FB_H], 0
  mov dword [BOOTINFO_ADDR + BI_FB_BPP], 0
  mov dword [BOOTINFO_ADDR + BI_FB_TYPE], 0
  mov dword [BOOTINFO_ADDR + BI_FONT_ADDR], 0
  mov dword [BOOTINFO_ADDR + BI_FONT_H], 0
  ret

e820_collect:
  pusha
  push ds
  push es

  xor ax, ax
  mov ds, ax
  mov es, ax

  xor ebx, ebx
  mov di, E820_ADDR

.e820_loop:
  mov eax, 0xE820
  mov edx, 0x534D4150
  mov ecx, 24
  mov dword [es:di + 20], 1
  int 0x15
  jc .done

  cmp eax, 0x534D4150
  jne .done

  inc dword [BOOTINFO_ADDR + BI_E820_COUNT]

  add di, 24
  cmp di, (E820_ADDR + E820_MAX*24)
  jae .done

  test ebx, ebx
  jne .e820_loop

.done:
  pop es
  pop ds
  popa
  ret

font_copy:
  pusha
  push ds
  push es

  mov ax, 0x1130
  mov bh, 0x06
  int 0x10

  test cx, cx
  jz .fail
  mov ax, es
  test ax, ax
  jz .fail

  mov ax, es
  mov ds, ax
  mov si, bp

  xor ax, ax
  mov es, ax
  mov di, FONT_ADDR

  mov bx, cx
  mov ax, bx
  shl ax, 8
  mov cx, ax
  rep movsb

  xor ax, ax
  mov ds, ax

  mov dword [BOOTINFO_ADDR + BI_FONT_ADDR], FONT_ADDR
  xor eax, eax
  mov ax, bx
  mov dword [BOOTINFO_ADDR + BI_FONT_H], eax

  pop es
  pop ds
  popa
  clc
  ret

.fail:
  pop es
  pop ds
  popa
  stc
  ret

; ------------------------------------------------------------
; Simple, safe VBE init
; Tries a few common good modes in order.
; ------------------------------------------------------------

vbe_init:
  push ds
  push es
  pusha

  xor ax, ax
  mov ds, ax
  mov es, ax

  mov si, vbe_modes

.try_next:
  lodsw
  test ax, ax
  jz .fail

  mov cx, ax
  call vbe_try_mode
  jc .try_next

  popa
  pop es
  pop ds
  clc
  ret

.fail:
  popa
  pop es
  pop ds
  stc
  ret

vbe_try_mode:
  pusha
  push ds
  push es

  call vbe_get_modeinfo
  jc .bad

  mov ax, [vbe_modeinfo + 0]
  test ax, 0x0001
  jz .bad
  test ax, 0x0080
  jz .bad

  mov al, [vbe_modeinfo + 0x19]
  cmp al, 16
  je .bpp_ok
  cmp al, 24
  je .bpp_ok
  cmp al, 32
  je .bpp_ok
  jmp .bad

.bpp_ok:
  mov al, [vbe_modeinfo + 0x1B]
  cmp al, 4
  je .mm_ok
  cmp al, 6
  je .mm_ok
  jmp .bad

.mm_ok:
  mov eax, [vbe_modeinfo + 0x28]
  test eax, eax
  jz .bad

  mov bx, cx
  or bx, 0x4000
  mov ax, 0x4F02
  int 0x10
  cmp ax, 0x004F
  jne .bad

  xor ax, ax
  mov ds, ax

  mov eax, [vbe_modeinfo + 0x28]
  mov [BOOTINFO_ADDR + BI_FB_ADDR], eax

  xor eax, eax
  mov ax, [vbe_modeinfo + 0x10]
  mov [BOOTINFO_ADDR + BI_FB_PITCH], eax

  xor eax, eax
  mov ax, [vbe_modeinfo + 0x12]
  mov [BOOTINFO_ADDR + BI_FB_W], eax

  xor eax, eax
  mov ax, [vbe_modeinfo + 0x14]
  mov [BOOTINFO_ADDR + BI_FB_H], eax

  xor eax, eax
  mov al, [vbe_modeinfo + 0x19]
  mov [BOOTINFO_ADDR + BI_FB_BPP], eax

  mov dword [BOOTINFO_ADDR + BI_FB_TYPE], 1

  pop es
  pop ds
  popa
  clc
  ret

.bad:
  pop es
  pop ds
  popa
  stc
  ret

vbe_get_modeinfo:
  pusha
  push ds
  push es

  xor ax, ax
  mov ds, ax
  mov es, ax

  mov ax, 0x4F01
  mov di, vbe_modeinfo
  int 0x10
  cmp ax, 0x004F
  jne .bad

  pop es
  pop ds
  popa
  clc
  ret

.bad:
  pop es
  pop ds
  popa
  stc
  ret

; ------------------------------------------------------------
; Text helpers
; ------------------------------------------------------------

print_hex8:
  pusha
  mov ah, al
  shr al, 4
  call print_hex_nibble
  mov al, ah
  and al, 0x0F
  call print_hex_nibble
  popa
  ret

print_hex_nibble:
  cmp al, 9
  jle .digit
  add al, 'A' - 10
  jmp .out
.digit:
  add al, '0'
.out:
  mov ah, 0x0E
  mov bh, 0
  mov bl, 0x07
  int 0x10
  ret

puts:
  pusha
.next:
  lodsb
  test al, al
  jz .done
  mov ah, 0x0E
  mov bh, 0
  mov bl, 0x07
  int 0x10
  jmp .next
.done:
  popa
  ret

; ------------------------------------------------------------
; A20
; ------------------------------------------------------------

a20_check:
  pushf
  cli
  push ds
  push es

  xor ax, ax
  mov ds, ax
  mov ax, 0xFFFF
  mov es, ax

  mov di, 0x0500
  mov si, 0x0510

  mov al, [ds:di]
  push ax
  mov al, [es:si]
  push ax

  mov byte [ds:di], 0x00
  mov byte [es:si], 0xFF

  mov al, [ds:di]
  cmp al, 0xFF
  jne .enabled

  pop ax
  mov [es:si], al
  pop ax
  mov [ds:di], al

  pop es
  pop ds
  popf
  xor ax, ax
  ret

.enabled:
  pop ax
  mov [es:si], al
  pop ax
  mov [ds:di], al

  pop es
  pop ds
  popf
  mov ax, 1
  ret

enable_a20:
  call a20_check
  cmp ax, 1
  je .done

  mov ax, 0x2401
  int 0x15

  call a20_check
  cmp ax, 1
  je .done

  in al, 0x92
  or al, 0x02
  and al, 0xFE
  out 0x92, al

  call a20_check
.done:
  ret

; ------------------------------------------------------------
; Data
; ------------------------------------------------------------

boot_drive db 0
int13_err  db 0

msg_stage2     db "Stage2: A20 + E820 + font + load kernel", 0x0D, 0x0A, 0
msg_a20_ok     db "A20 OK", 0x0D, 0x0A, 0
msg_a20_fail   db "A20 FAIL", 0x0D, 0x0A, 0
msg_e820_ok    db "E820 OK", 0x0D, 0x0A, 0
msg_e820_fail  db "E820 unavailable", 0x0D, 0x0A, 0
msg_font       db "Copying BIOS 8x16 font...", 0x0D, 0x0A, 0
msg_font_ok    db "Font OK", 0x0D, 0x0A, 0
msg_font_fail  db "Font unavailable", 0x0D, 0x0A, 0
msg_loadk      db "Loading kernel (to 0x00010000)...", 0x0D, 0x0A, 0
msg_read_ok    db "Kernel read OK", 0x0D, 0x0A, 0
msg_read_fail  db "Kernel read FAIL. INT13 AH=0x", 0
msg_video_try  db "Video: trying VBE LFB...", 0x0D, 0x0A, 0
msg_video_ok   db "Video: VBE OK", 0x0D, 0x0A, 0
msg_video_fail db "Video: VBE not available", 0x0D, 0x0A, 0
msg_pm         db "Entering protected mode...", 0x0D, 0x0A, 0
msg_nl         db 0x0D, 0x0A, 0

; ordered best -> acceptable
vbe_modes:
  dw 0x11B    ; 1280x1024x32
  dw 0x118    ; 1024x768x32
  dw 0x117    ; 1024x768x16
  dw 0x115    ; 800x600x24
  dw 0x112    ; 640x480x24
  dw 0

vbe_modeinfo:
  times 256 db 0

dap:
  db 0x10, 0x00
  dw 0
  dw 0
  dw 0
  dq 0

gdt_start:
gdt_null: dq 0x0000000000000000
gdt_code: dq 0x00CF9A000000FFFF
gdt_data: dq 0x00CF92000000FFFF
gdt_end:

gdt_desc:
  dw gdt_end - gdt_start - 1
  dd gdt_start