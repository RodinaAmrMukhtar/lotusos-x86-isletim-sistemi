#pragma once
#include <stdint.h>

#define MB2_BOOTLOADER_MAGIC 0x36d76289u

/* Multiboot2 info tags */
#define MB2_TAG_END            0
#define MB2_TAG_CMDLINE        1
#define MB2_TAG_BOOT_LOADER    2
#define MB2_TAG_BASIC_MEMINFO  4
#define MB2_TAG_MMAP           6
#define MB2_TAG_FRAMEBUFFER    8

/* Multiboot2 memory map types */
#define MB2_MMAP_TYPE_AVAILABLE 1

typedef struct __attribute__((packed)) mb2_info_header {
  uint32_t total_size;
  uint32_t reserved;
} mb2_info_header_t;

typedef struct __attribute__((packed)) mb2_tag {
  uint32_t type;
  uint32_t size;
} mb2_tag_t;

typedef struct __attribute__((packed)) mb2_tag_framebuffer {
  uint32_t type;
  uint32_t size;
  uint64_t framebuffer_addr;
  uint32_t framebuffer_pitch;
  uint32_t framebuffer_width;
  uint32_t framebuffer_height;
  uint8_t  framebuffer_bpp;
  uint8_t  framebuffer_type;
  uint16_t reserved;
  /* followed by color info depending on type */
} mb2_tag_framebuffer_t;

typedef struct __attribute__((packed)) mb2_tag_mmap {
  uint32_t type;
  uint32_t size;
  uint32_t entry_size;
  uint32_t entry_version;
  /* followed by entries */
} mb2_tag_mmap_t;

typedef struct __attribute__((packed)) mb2_mmap_entry {
  uint64_t addr;
  uint64_t len;
  uint32_t type;
  uint32_t zero;
} mb2_mmap_entry_t;
