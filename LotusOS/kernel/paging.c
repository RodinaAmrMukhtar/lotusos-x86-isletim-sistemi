#include "paging.h"
#include "pmm.h"
#include <stdint.h>

#define PAGE_SIZE 4096u

static uint32_t g_pd_phys = 0;

static void memzero(uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

static inline void invlpg(uint32_t addr) {
  __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static uint32_t* pd_ptr(void) {
  return (uint32_t*)(uintptr_t)g_pd_phys; /* identity-mapped */
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
  uint32_t pd_i = (virt >> 22) & 0x3FFu;
  uint32_t pt_i = (virt >> 12) & 0x3FFu;

  uint32_t* pd = pd_ptr();

  if (!(pd[pd_i] & PAGE_PRESENT)) {
    uint32_t pt_phys = pmm_alloc_frame();
    if (!pt_phys) return;
    memzero((uint8_t*)(uintptr_t)pt_phys, PAGE_SIZE);

    /* IMPORTANT: PDE must carry USER bit if we want user pages inside this PDE */
    pd[pd_i] = (pt_phys & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
  } else {
    /* IMPORTANT FIX:
       If PDE already exists (identity-mapped), and we map a user page inside it,
       we MUST set PDE.USER=1 too, otherwise Ring3 will fault with protection-violation.
    */
    if (flags & PAGE_USER) {
      pd[pd_i] |= PAGE_USER;
    }
  }

  uint32_t pt_phys = pd[pd_i] & 0xFFFFF000u;
  uint32_t* pt = (uint32_t*)(uintptr_t)pt_phys; /* identity-mapped */

  /* PTE must carry USER too */
  pt[pt_i] = (phys & 0xFFFFF000u) | (flags & (PAGE_USER | PAGE_RW)) | PAGE_PRESENT;
  invlpg(virt);
}

static void enable_paging(uint32_t pd_phys) {
  __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
  uint32_t cr0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 |= 0x80000000u; /* PG */
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void paging_init_identity(uint32_t map_mb) {
  if (map_mb < 4) map_mb = 4;
  uint32_t tables = (map_mb + 3u) / 4u; /* each PT maps 4MB */
  if (tables > 1024u) tables = 1024u;

  g_pd_phys = pmm_alloc_frame();
  if (!g_pd_phys) return;

  memzero((uint8_t*)(uintptr_t)g_pd_phys, PAGE_SIZE);
  uint32_t* pd = pd_ptr();

  for (uint32_t t = 0; t < tables; t++) {
    uint32_t pt_phys = pmm_alloc_frame();
    if (!pt_phys) break;
    memzero((uint8_t*)(uintptr_t)pt_phys, PAGE_SIZE);

    uint32_t* pt = (uint32_t*)(uintptr_t)pt_phys;

    uint32_t base = t * 0x400000u; /* 4MB */
    for (uint32_t i = 0; i < 1024u; i++) {
      uint32_t addr = base + i * PAGE_SIZE;
      pt[i] = (addr & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    }

    pd[t] = (pt_phys & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
  }

  enable_paging(g_pd_phys);
}

uint32_t paging_cr3(void) { return g_pd_phys; }