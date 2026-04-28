; stage1.asm - BIOS boot sector + MBR partition table
; Loads stage2 from LBA 1 into 0000:8000 and jumps.
; Also contains an MBR partition entry for a FAT32 partition.

bits 16
org 0x7C00

%define STAGE2_LOAD_SEG 0x0000
%define STAGE2_LOAD_OFF 0x8000

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 32
%endif

%ifndef PART_LBA
%define PART_LBA 2048
%endif

%ifndef PART_SECTORS
%define PART_SECTORS 131072
%endif

start:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7C00
  sti

  mov [boot_drive], dl

  ; INT13 extensions check
  mov dl, [boot_drive]
  mov ah, 0x41
  mov bx, 0x55AA
  int 0x13
  jc disk_fail
  cmp bx, 0xAA55
  jne disk_fail
  test cx, 1
  jz disk_fail

  ; Read stage2: LBA=1, count=STAGE2_SECTORS -> 0000:8000
  mov word [dap + 2], STAGE2_SECTORS
  mov word [dap + 4], STAGE2_LOAD_OFF
  mov word [dap + 6], STAGE2_LOAD_SEG
  mov dword [dap + 8], 1
  mov dword [dap + 12], 0

  mov dl, [boot_drive]
  mov ah, 0x42
  mov si, dap
  int 0x13
  jc disk_fail

  jmp STAGE2_LOAD_SEG:STAGE2_LOAD_OFF

disk_fail:
  ; hang
  cli
.h:
  hlt
  jmp .h

boot_drive: db 0

dap:
  db 0x10, 0x00
  dw 0
  dw 0
  dw 0
  dq 0

; Pad boot code to 446 bytes (MBR partition table starts at 0x1BE)
times 446-($-$$) db 0

; ---------------- MBR Partition Table (4 entries, 16 bytes each) ----------------
; One FAT32 (LBA) partition starting at PART_LBA, length PART_SECTORS.
; status, CHS first (ignored), type, CHS last (ignored), LBA first, sectors
partition_table:
  db 0x00              ; status (0x80 bootable, 0x00 normal)
  db 0x00,0x02,0x00     ; CHS first (dummy)
  db 0x0C              ; type: FAT32 LBA
  db 0xFF,0xFF,0xFF     ; CHS last (dummy)
  dd PART_LBA
  dd PART_SECTORS

  times 16*3 db 0      ; remaining 3 entries empty

; Signature
dw 0xAA55