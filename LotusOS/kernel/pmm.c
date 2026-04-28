#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096u

/* Cap to keep bitmap small (good for a class/hobby OS). Increase later if needed. */
#define MAX_PHYS_MEM (512u * 1024u * 1024u) /* 512 MB */
#define MAX_FRAMES   (MAX_PHYS_MEM / PAGE_SIZE)
#define BITMAP_WORDS (MAX_FRAMES / 32u)

static uint32_t g_bitmap[BITMAP_WORDS];
static uint32_t g_max_frame = 0;      /* number of frames we consider */
static uint32_t g_max_phys  = 0;      /* max physical addr (bytes) */
static uint32_t g_total_kb  = 0;      /* usable RAM from E820 */
static uint32_t g_free_kb   = 0;

static inline void bm_set(uint32_t frame) {
  g_bitmap[frame >> 5] |= (1u << (frame & 31u));
}
static inline void bm_clear(uint32_t frame) {
  g_bitmap[frame >> 5] &= ~(1u << (frame & 31u));
}
static inline int bm_test(uint32_t frame) {
  return (g_bitmap[frame >> 5] >> (frame & 31u)) & 1u;
}

static uint32_t align_up(uint32_t v, uint32_t a) {
  return (v + a - 1u) & ~(a - 1u);
}

static void reserve_range(uint32_t start, uint32_t end) {
  if (end <= start) return;
  if (start >= MAX_PHYS_MEM) return;
  if (end > MAX_PHYS_MEM) end = MAX_PHYS_MEM;

  uint32_t s = start / PAGE_SIZE;
  uint32_t e = (end + PAGE_SIZE - 1u) / PAGE_SIZE;
  if (e > g_max_frame) e = g_max_frame;

  for (uint32_t f = s; f < e; f++) {
    if (!bm_test(f)) bm_set(f);
  }
}

static void free_range(uint32_t start, uint32_t end) {
  if (end <= start) return;
  if (start >= MAX_PHYS_MEM) return;
  if (end > MAX_PHYS_MEM) end = MAX_PHYS_MEM;

  uint32_t s = align_up(start, PAGE_SIZE) / PAGE_SIZE;
  uint32_t e = (end / PAGE_SIZE);
  if (e > g_max_frame) e = g_max_frame;

  for (uint32_t f = s; f < e; f++) bm_clear(f);
}

static void recompute_free_kb(void) {
  uint32_t free_frames = 0;
  for (uint32_t f = 0; f < g_max_frame; f++) {
    if (!bm_test(f)) free_frames++;
  }
  g_free_kb = free_frames * 4u;
}

void pmm_init(const boot_info_t* bi,
              uint32_t kernel_start, uint32_t kernel_end,
              uint32_t heap_start, uint32_t heap_end) {

  /* Default: everything used */
  for (uint32_t i = 0; i < BITMAP_WORDS; i++) g_bitmap[i] = 0xFFFFFFFFu;

  g_max_phys = 0;
  g_total_kb = 0;

  /* If E820 is present, mark usable ranges as free */
  if (bi && bi->magic == BOOTINFO_MAGIC && bi->e820_count > 0 && bi->e820_addr != 0) {
    const e820_entry_t* e = (const e820_entry_t*)(uintptr_t)bi->e820_addr;

    /* Compute max phys + usable total */
    for (uint32_t i = 0; i < bi->e820_count; i++) {
      uint64_t base = e[i].base;
      uint64_t len  = e[i].length;
      uint64_t end  = base + len;

      if (end > g_max_phys) {
        uint64_t capped = end;
        if (capped > (uint64_t)MAX_PHYS_MEM) capped = (uint64_t)MAX_PHYS_MEM;
        g_max_phys = (uint32_t)capped;
      }

      if (e[i].type == 1 && len) {
        uint64_t capped_len = len;
        if (base >= (uint64_t)MAX_PHYS_MEM) continue;
        if (base + capped_len > (uint64_t)MAX_PHYS_MEM) capped_len = (uint64_t)MAX_PHYS_MEM - base;
        g_total_kb += (uint32_t)(capped_len / 1024ull);
      }
    }
  } else {
    /* Fallback: assume 64MB usable RAM (common in QEMU) */
    g_max_phys = 64u * 1024u * 1024u;
    g_total_kb = 64u * 1024u;
  }

  if (g_max_phys < (16u * 1024u * 1024u)) {
    g_max_phys = 16u * 1024u * 1024u;
  }

  g_max_frame = g_max_phys / PAGE_SIZE;
  if (g_max_frame > MAX_FRAMES) g_max_frame = MAX_FRAMES;

  /* Free usable memory ranges */
  if (bi && bi->magic == BOOTINFO_MAGIC && bi->e820_count > 0 && bi->e820_addr != 0) {
    const e820_entry_t* e = (const e820_entry_t*)(uintptr_t)bi->e820_addr;
    for (uint32_t i = 0; i < bi->e820_count; i++) {
      if (e[i].type != 1) continue;
      uint64_t base = e[i].base;
      uint64_t end  = e[i].base + e[i].length;
      if (base >= (uint64_t)MAX_PHYS_MEM) continue;
      if (end > (uint64_t)MAX_PHYS_MEM) end = (uint64_t)MAX_PHYS_MEM;
      free_range((uint32_t)base, (uint32_t)end);
    }
  } else {
    /* fallback: 1MB..64MB usable */
    free_range(0x00100000u, g_max_phys);
  }

  /* Reserve low memory + our kernel + heap region */
  reserve_range(0x00000000u, 0x00100000u);
  reserve_range(kernel_start, kernel_end);
  reserve_range(heap_start, heap_end);

  /* ✅ CRITICAL: reserve framebuffer physical region so PMM never allocates it */
  if (bi && bi->magic == BOOTINFO_MAGIC && bi->fb_addr && bi->fb_pitch && bi->fb_height) {
    uint64_t fb_base = (uint64_t)(bi->fb_addr & 0xFFFFF000u);
    uint64_t fb_size = (uint64_t)bi->fb_pitch * (uint64_t)bi->fb_height;
    uint64_t fb_end  = fb_base + ((fb_size + 0xFFFu) & ~0xFFFu);
    if (fb_end > fb_base) {
      reserve_range((uint32_t)fb_base, (uint32_t)fb_end);
    }
  }

  recompute_free_kb();
}

uint32_t pmm_alloc_frame(void) {
  for (uint32_t w = 0; w < (g_max_frame + 31u) / 32u; w++) {
    uint32_t val = g_bitmap[w];
    if (val == 0xFFFFFFFFu) continue;

    for (uint32_t b = 0; b < 32u; b++) {
      uint32_t frame = (w << 5) + b;
      if (frame >= g_max_frame) return 0;

      if (!((val >> b) & 1u)) {
        bm_set(frame);
        g_free_kb -= 4u;
        return frame * PAGE_SIZE;
      }
    }
  }
  return 0;
}

void pmm_free_frame(uint32_t phys) {
  uint32_t frame = phys / PAGE_SIZE;
  if (frame >= g_max_frame) return;
  if (!bm_test(frame)) return;
  bm_clear(frame);
  g_free_kb += 4u;
}

uint32_t pmm_total_kb(void) { return g_total_kb; }
uint32_t pmm_free_kb(void)  { return g_free_kb; }
uint32_t pmm_max_phys(void) { return g_max_phys; }