#include "multiboot2.h"
#include "bootinfo.h"

/*
  Convert Multiboot2 info into your existing boot_info_t so the rest of your kernel
  can stay unchanged (gfx_init, pmm_init, etc).
*/
static boot_info_t g_bi;
static e820_entry_t g_e820[128];

static uint32_t align8(uint32_t x) { return (x + 7u) & ~7u; }

boot_info_t* bootinfo_from_multiboot2(uint32_t mb2_info_phys) {
  g_bi.magic = BOOTINFO_MAGIC;
  g_bi.e820_count = 0;
  g_bi.e820_addr = (uint32_t)(uintptr_t)&g_e820[0];
  g_bi.boot_drive = 0x80; /* ATA driver doesn't use this, keep a sane default */

  g_bi.fb_addr = 0;
  g_bi.fb_pitch = 0;
  g_bi.fb_width = 0;
  g_bi.fb_height = 0;
  g_bi.fb_bpp = 0;
  g_bi.fb_type = 0;

  g_bi.font_addr = 0;
  g_bi.font_height = 16;

  if (!mb2_info_phys) return &g_bi;

  const mb2_info_header_t* hdr = (const mb2_info_header_t*)(uintptr_t)mb2_info_phys;
  uint32_t total = hdr->total_size;
  uint32_t off = sizeof(mb2_info_header_t);

  while (off + sizeof(mb2_tag_t) <= total) {
    const mb2_tag_t* tag = (const mb2_tag_t*)(uintptr_t)(mb2_info_phys + off);
    if (tag->type == MB2_TAG_END) break;
    if (tag->size < sizeof(mb2_tag_t)) break;

    if (tag->type == MB2_TAG_FRAMEBUFFER && tag->size >= sizeof(mb2_tag_framebuffer_t)) {
      const mb2_tag_framebuffer_t* fb = (const mb2_tag_framebuffer_t*)tag;
      uint64_t addr64 = fb->framebuffer_addr;
      /* On i386, we only handle <4GiB framebuffer addresses */
      if (addr64 <= 0xFFFFFFFFull) {
        g_bi.fb_addr = (uint32_t)addr64;
        g_bi.fb_pitch = fb->framebuffer_pitch;
        g_bi.fb_width = fb->framebuffer_width;
        g_bi.fb_height = fb->framebuffer_height;
        g_bi.fb_bpp = fb->framebuffer_bpp;
        g_bi.fb_type = (fb->framebuffer_type == 1) ? 1 : 0; /* 1 = RGB */
      }
    }

    if (tag->type == MB2_TAG_MMAP && tag->size >= sizeof(mb2_tag_mmap_t)) {
      const mb2_tag_mmap_t* mm = (const mb2_tag_mmap_t*)tag;
      uint32_t entsz = mm->entry_size;
      if (entsz < sizeof(mb2_mmap_entry_t)) entsz = sizeof(mb2_mmap_entry_t);

      uint32_t mm_off = sizeof(mb2_tag_mmap_t);
      while (mm_off + sizeof(mb2_mmap_entry_t) <= tag->size && g_bi.e820_count < 128) {
        const mb2_mmap_entry_t* e = (const mb2_mmap_entry_t*)((const uint8_t*)tag + mm_off);

        g_e820[g_bi.e820_count].base   = e->addr;
        g_e820[g_bi.e820_count].length = e->len;
        g_e820[g_bi.e820_count].type   = (e->type == MB2_MMAP_TYPE_AVAILABLE) ? 1 : 2;
        g_e820[g_bi.e820_count].acpi_ext = 0;
        g_bi.e820_count++;

        mm_off += entsz;
      }
    }

    off += align8(tag->size);
  }

  return &g_bi;
}
