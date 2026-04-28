#pragma once
#include <stdint.h>

#define BOOTINFO_MAGIC 0x544F4F42u /* 'BOOT' */

typedef struct __attribute__((packed)) e820_entry {
  uint64_t base;
  uint64_t length;
  uint32_t type;     /* 1 = usable RAM */
  uint32_t acpi_ext; /* extended attributes, may be 0 */
} e820_entry_t;

/*
  boot_info_t is written by the bootloader at physical 0x00005000 and passed to kmain in EAX.

  Notes:
  - All addresses are PHYSICAL addresses.
  - Framebuffer fields are filled only if stage2 successfully switches to a VBE linear-framebuffer mode.
  - font_addr/font_height are filled if stage2 copied the BIOS 8x16 font into low RAM.
*/
typedef struct __attribute__((packed)) boot_info {
  uint32_t magic;
  uint32_t e820_count;
  uint32_t e820_addr;   /* physical address of e820_entry_t[] */
  uint32_t boot_drive;  /* BIOS drive number (0x80 = first HDD) */

  /* VBE linear framebuffer (0 = not available) */
  uint32_t fb_addr;     /* physical base address */
  uint32_t fb_pitch;    /* bytes per scanline */
  uint32_t fb_width;    /* pixels */
  uint32_t fb_height;   /* pixels */
  uint32_t fb_bpp;      /* bits per pixel (usually 32 or 16) */
  uint32_t fb_type;     /* 0=none, 1=RGB */

  /* BIOS font copied to low RAM (0 = not available) */
  uint32_t font_addr;   /* physical address of 256 * font_height bytes */
  uint32_t font_height; /* bytes per character (usually 16 for 8x16) */
} boot_info_t;