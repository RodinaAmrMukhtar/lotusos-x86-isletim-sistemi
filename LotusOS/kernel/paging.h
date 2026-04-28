#pragma once
#include <stdint.h>

#define PAGE_PRESENT 0x001u
#define PAGE_RW      0x002u
#define PAGE_USER    0x004u

void paging_init_identity(uint32_t map_mb);
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
uint32_t paging_cr3(void);