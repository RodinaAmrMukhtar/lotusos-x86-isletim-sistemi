FS_LBA := 2048
FS_SECTORS := 131072
FS_LABEL := MYOSFS

DISK_SECTORS := 200000

CC := gcc
CXX := g++
LD := ld
NASM := nasm

CFLAGS := -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -nostdlib -O2 -Wall -Wextra
CXXFLAGS := -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -nostdlib -O2 -Wall -Wextra \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -std=gnu++17

KERNEL_LDFLAGS := -m32 -nostdlib -T kernel/linker.ld -no-pie
KERNEL_LIBS := -lgcc

APP_CFLAGS := -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -nostdlib -O2
APP_LDFLAGS := -m elf_i386 -nostdlib

BUILD := build
IMG := $(BUILD)/os.img
FSIMG := $(BUILD)/fs.img

ISO := $(BUILD)/myos.iso
ISO_ROOT := $(BUILD)/iso_root

# System package location for Limine data files.
LIMINE_SHARE ?= /usr/share/limine

# Local fallback: auto-download/build official Limine binary release here when the
# host utility is missing on the system.
LIMINE_DIR ?= limine
LIMINE_REPO ?= https://github.com/limine-bootloader/limine.git
LIMINE_BRANCH ?= v11.x-binary
LIMINE_STAMP := $(LIMINE_DIR)/.host-built

# Optional manual override:
#   make LIMINE_BIN=/full/path/to/limine run
#   make LIMINE_BIN=/full/path/to/limine-deploy run
LIMINE_BIN ?=

SYSTEM_LIMINE_BIOS_SYS := $(wildcard $(LIMINE_SHARE)/limine-bios.sys)
SYSTEM_LIMINE_BIOS_CD  := $(wildcard $(LIMINE_SHARE)/limine-bios-cd.bin)
LOCAL_LIMINE_BIOS_SYS  := $(wildcard $(LIMINE_DIR)/limine-bios.sys)
LOCAL_LIMINE_BIOS_CD   := $(wildcard $(LIMINE_DIR)/limine-bios-cd.bin)

ifeq ($(strip $(LIMINE_BIN)),)
  ifneq ($(shell command -v limine 2>/dev/null),)
    LIMINE_BIN := limine
  else ifneq ($(shell command -v limine-deploy 2>/dev/null),)
    LIMINE_BIN := limine-deploy
  else ifneq ($(wildcard $(LIMINE_DIR)/limine),)
    LIMINE_BIN := $(LIMINE_DIR)/limine
  else
    LIMINE_BIN := $(LIMINE_DIR)/limine
    NEED_LOCAL_LIMINE := yes
  endif
endif

ifeq ($(and $(SYSTEM_LIMINE_BIOS_SYS),$(SYSTEM_LIMINE_BIOS_CD)),)
  LIMINE_FILES_DIR := $(LIMINE_DIR)
  NEED_LOCAL_LIMINE := yes
else
  LIMINE_FILES_DIR := $(LIMINE_SHARE)
endif

ifeq ($(NEED_LOCAL_LIMINE),yes)
  LIMINE_PREREQ := $(LIMINE_STAMP)
else
  LIMINE_PREREQ :=
endif

KERNEL_C := \
	kernel/kernel.c kernel/terminal.c kernel/gdt.c kernel/idt.c kernel/isr.c kernel/syscall.c kernel/pic.c \
	kernel/keyboard.c kernel/shell.c kernel/timer.c kernel/disk.c \
	kernel/mbr.c kernel/fat32.c kernel/vfs.c \
	kernel/pmm.c kernel/paging.c kernel/kheap.c \
	kernel/task.c kernel/elf.c \
	kernel/events.c kernel/mouse.c \
	kernel/splash.c kernel/logo.c \
	kernel/multiboot2.c \
	kernel/gfx.cpp kernel/wm.cpp kernel/ui.cpp

all: update

$(BUILD):
	mkdir -p $(BUILD)

# ---------- Limine fallback bootstrap ----------
$(LIMINE_STAMP):
	@set -e; \
	if ! command -v git >/dev/null 2>&1; then \
	  echo "ERROR: git is required to fetch the local Limine fallback."; \
	  exit 1; \
	fi; \
	if [ ! -d "$(LIMINE_DIR)" ]; then \
	  echo "Fetching Limine $(LIMINE_BRANCH) into $(LIMINE_DIR) ..."; \
	  git clone --branch "$(LIMINE_BRANCH)" --depth=1 "$(LIMINE_REPO)" "$(LIMINE_DIR)"; \
	fi; \
	$(MAKE) -C "$(LIMINE_DIR)"; \
	if [ ! -x "$(LIMINE_DIR)/limine" ]; then \
	  echo "ERROR: local Limine build finished, but $(LIMINE_DIR)/limine was not produced."; \
	  exit 1; \
	fi; \
	touch "$@"

# ---------- Kernel (Multiboot2) ----------
$(BUILD)/kernel.elf: kernel/multiboot2_header.asm kernel/entry.asm kernel/isr.asm $(KERNEL_C) kernel/linker.ld | $(BUILD)
	$(NASM) -f elf32 kernel/multiboot2_header.asm -o $(BUILD)/multiboot2_header.o
	$(NASM) -f elf32 kernel/entry.asm -o $(BUILD)/entry.o
	$(NASM) -f elf32 kernel/isr.asm   -o $(BUILD)/isr.o

	$(CC) $(CFLAGS) -Ikernel -c kernel/kernel.c     -o $(BUILD)/kernel.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/terminal.c   -o $(BUILD)/terminal.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/gdt.c        -o $(BUILD)/gdt.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/idt.c        -o $(BUILD)/idt.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/isr.c        -o $(BUILD)/isr_c.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/syscall.c    -o $(BUILD)/syscall.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/pic.c        -o $(BUILD)/pic.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/keyboard.c   -o $(BUILD)/keyboard.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/shell.c      -o $(BUILD)/shell.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/timer.c      -o $(BUILD)/timer.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/disk.c       -o $(BUILD)/disk.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/mbr.c        -o $(BUILD)/mbr.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/fat32.c      -o $(BUILD)/fat32.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/vfs.c        -o $(BUILD)/vfs.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/pmm.c        -o $(BUILD)/pmm.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/paging.c     -o $(BUILD)/paging.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/kheap.c      -o $(BUILD)/kheap.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/task.c       -o $(BUILD)/task.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/elf.c        -o $(BUILD)/elf.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/events.c     -o $(BUILD)/events.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/mouse.c      -o $(BUILD)/mouse.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/splash.c     -o $(BUILD)/splash.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/logo.c       -o $(BUILD)/logo.o
	$(CC) $(CFLAGS) -Ikernel -c kernel/multiboot2.c -o $(BUILD)/multiboot2.o

	$(CXX) $(CXXFLAGS) -Ikernel -c kernel/gfx.cpp   -o $(BUILD)/gfx.o
	$(CXX) $(CXXFLAGS) -Ikernel -c kernel/wm.cpp    -o $(BUILD)/wm.o
	$(CXX) $(CXXFLAGS) -Ikernel -c kernel/ui.cpp    -o $(BUILD)/ui.o

	$(CC) $(KERNEL_LDFLAGS) \
	  $(BUILD)/multiboot2_header.o $(BUILD)/entry.o $(BUILD)/isr.o \
	  $(BUILD)/kernel.o $(BUILD)/terminal.o $(BUILD)/gdt.o $(BUILD)/idt.o $(BUILD)/isr_c.o $(BUILD)/syscall.o \
	  $(BUILD)/pic.o $(BUILD)/keyboard.o $(BUILD)/shell.o $(BUILD)/timer.o \
	  $(BUILD)/disk.o $(BUILD)/mbr.o $(BUILD)/fat32.o $(BUILD)/vfs.o \
	  $(BUILD)/pmm.o $(BUILD)/paging.o $(BUILD)/kheap.o \
	  $(BUILD)/task.o $(BUILD)/elf.o \
	  $(BUILD)/events.o $(BUILD)/mouse.o \
	  $(BUILD)/splash.o $(BUILD)/logo.o \
	  $(BUILD)/multiboot2.o \
	  $(BUILD)/gfx.o $(BUILD)/wm.o $(BUILD)/ui.o \
	  -o $(BUILD)/kernel.elf $(KERNEL_LIBS)

# ---------- Apps ----------
$(BUILD)/hello.o: apps/hello.c | $(BUILD)
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/HELLO.ELF: $(BUILD)/hello.o apps/hello.ld | $(BUILD)
	$(LD) $(APP_LDFLAGS) -T apps/hello.ld -o $@ $(BUILD)/hello.o

files/HELLO.ELF: $(BUILD)/HELLO.ELF
	cp $(BUILD)/HELLO.ELF files/HELLO.ELF

# ---------- Filesystem (data disk content) ----------
$(FSIMG): files/HELLO.ELF | $(BUILD)
	dd if=/dev/zero of=$(FSIMG) bs=512 count=$(FS_SECTORS) status=none
	mkfs.fat -F 32 -n "$(FS_LABEL)" $(FSIMG) >/dev/null
	@for f in files/*; do \
	  case "$$f" in \
	    *:Zone.Identifier|*.DS_Store) continue ;; \
	  esac; \
	  if [ -f "$$f" ]; then \
	    echo "Copying $$f -> FAT32"; \
	    mcopy -i $(FSIMG) -o "$$f" "::/`basename $$f`"; \
	  fi; \
	done

# Data disk image (WIPES FS)
$(IMG): $(FSIMG) | $(BUILD)
	dd if=/dev/zero of=$(IMG) bs=512 count=$(DISK_SECTORS) status=none
	dd if=$(FSIMG) of=$(IMG) conv=notrunc bs=512 seek=$(FS_LBA) status=none
	@echo "Built NEW data disk: $(IMG) (FAT32 written at LBA $(FS_LBA))"

# ---------- Limine ISO (BIOS) ----------
$(ISO_ROOT): | $(BUILD)
	mkdir -p $(ISO_ROOT)

$(ISO_ROOT)/limine.conf: boot/limine.conf | $(ISO_ROOT)
	cp $< $@

$(ISO_ROOT)/kernel.elf: $(BUILD)/kernel.elf | $(ISO_ROOT)
	cp $< $@

$(ISO_ROOT)/limine-bios.sys: $(LIMINE_PREREQ) | $(ISO_ROOT)
	cp "$(LIMINE_FILES_DIR)/limine-bios.sys" $@

$(ISO_ROOT)/limine-bios-cd.bin: $(LIMINE_PREREQ) | $(ISO_ROOT)
	cp "$(LIMINE_FILES_DIR)/limine-bios-cd.bin" $@

$(ISO): $(LIMINE_PREREQ) $(ISO_ROOT)/limine.conf $(ISO_ROOT)/kernel.elf $(ISO_ROOT)/limine-bios.sys $(ISO_ROOT)/limine-bios-cd.bin | $(BUILD)
	xorriso -as mkisofs -R -r -J \
	  -b limine-bios-cd.bin \
	  -no-emul-boot -boot-load-size 4 -boot-info-table \
	  $(ISO_ROOT) -o $(ISO)
	@set -e; \
	case "$(notdir $(LIMINE_BIN))" in \
	  limine-deploy) $(LIMINE_BIN) $(ISO) ;; \
	  *) $(LIMINE_BIN) bios-install $(ISO) ;; \
	esac

# ---------- Convenience targets ----------
# Default dev flow: rebuild kernel + ISO only (FS preserved).
update: $(ISO)
	@if [ ! -f "$(IMG)" ]; then \
	  echo "No data disk yet -> making fresh image first"; \
	  $(MAKE) fresh; \
	fi
	@echo "Updated kernel + Limine ISO: $(ISO) (data disk preserved: $(IMG))"

fresh: $(IMG) $(ISO)

run: update
	qemu-system-i386 -m 256M -vga std -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot -boot d -cdrom $(ISO) -drive if=ide,format=raw,file=$(IMG)

clean:
	rm -rf $(BUILD)
	rm -f files/HELLO.ELF

clean-limine:
	rm -rf $(LIMINE_DIR)

.PHONY: all run clean clean-limine fresh update
