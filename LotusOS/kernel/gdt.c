#include "gdt.h"

/*
  We booted into protected mode using Stage2's tiny GDT.
  For a "real" OS we need user segments + a TSS so Ring3 -> Ring0 transitions work.

  Layout:
    0: null
    1: kernel code (0x08)
    2: kernel data (0x10)
    3: user   code (0x18) => selector 0x1B with RPL=3
    4: user   data (0x20) => selector 0x23 with RPL=3
    5: TSS (0x28)
*/

typedef struct {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  gran;
  uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
  uint16_t limit;
  uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

/* 32-bit TSS */
typedef struct {
  uint32_t prev_tss;
  uint32_t esp0;
  uint32_t ss0;
  uint32_t esp1;
  uint32_t ss1;
  uint32_t esp2;
  uint32_t ss2;
  uint32_t cr3;
  uint32_t eip;
  uint32_t eflags;
  uint32_t eax, ecx, edx, ebx;
  uint32_t esp, ebp, esi, edi;
  uint32_t es, cs, ss, ds, fs, gs;
  uint32_t ldt;
  uint16_t trap;
  uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static gdt_entry_t g_gdt[6];
static gdt_ptr_t   g_gdtp;
static tss_t       g_tss;

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
  g_gdt[idx].limit_low = (uint16_t)(limit & 0xFFFFu);
  g_gdt[idx].base_low  = (uint16_t)(base & 0xFFFFu);
  g_gdt[idx].base_mid  = (uint8_t)((base >> 16) & 0xFFu);
  g_gdt[idx].access    = access;
  g_gdt[idx].gran      = (uint8_t)(((limit >> 16) & 0x0Fu) | (gran & 0xF0u));
  g_gdt[idx].base_high = (uint8_t)((base >> 24) & 0xFFu);
}

static void memzero(uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

void tss_set_esp0(uint32_t esp0) {
  g_tss.esp0 = esp0;
}

static void tss_init(uint32_t esp0) {
  memzero((uint8_t*)&g_tss, (uint32_t)sizeof(g_tss));
  g_tss.ss0 = GDT_KERNEL_DS;
  g_tss.esp0 = esp0;
  g_tss.iomap_base = (uint16_t)sizeof(g_tss);
}

void gdt_init(void) {
  /* Null */
  gdt_set_entry(0, 0, 0, 0, 0);

  /* Kernel code/data: base 0, limit 4GB */
  gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);
  gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);

  /* User code/data */
  gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);
  gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);

  /* TSS */
  uint32_t esp;
  __asm__ volatile("mov %%esp, %0" : "=r"(esp));
  tss_init(esp);

  uint32_t tss_base  = (uint32_t)(uintptr_t)&g_tss;
  uint32_t tss_limit = (uint32_t)sizeof(g_tss) - 1u;
  /* access=0x89: present, ring0, type=9 (available 32-bit TSS) */
  gdt_set_entry(5, tss_base, tss_limit, 0x89, 0x00);

  g_gdtp.limit = (uint16_t)(sizeof(g_gdt) - 1u);
  g_gdtp.base  = (uint32_t)(uintptr_t)&g_gdt;

  /* Load GDT, reload segments, load TSS.
     IMPORTANT: do NOT STI here. Kernel will STI only when IDT+PIC are ready. */
  __asm__ volatile(
    "cli\n"
    "lgdt (%0)\n"
    "mov %1, %%ax\n"
    "mov %%ax, %%ds\n"
    "mov %%ax, %%es\n"
    "mov %%ax, %%fs\n"
    "mov %%ax, %%gs\n"
    "mov %%ax, %%ss\n"
    "ljmp $0x08, $1f\n"
    "1:\n"
    "mov %2, %%ax\n"
    "ltr %%ax\n"
    :
    : "r"(&g_gdtp), "r"((uint16_t)GDT_KERNEL_DS), "r"((uint16_t)GDT_TSS_SEL)
    : "ax", "memory"
  );
}