#pragma once
#include <stdint.h>
#include "bootinfo.h"

/* Physical Memory Manager (4KB frames) */
void pmm_init(const boot_info_t* bi,
              uint32_t kernel_start, uint32_t kernel_end,
              uint32_t heap_start, uint32_t heap_end);

uint32_t pmm_alloc_frame(void);   /* returns physical address, 0 on failure */
void     pmm_free_frame(uint32_t phys);

uint32_t pmm_total_kb(void);      /* usable RAM (KB) */
uint32_t pmm_free_kb(void);       /* currently free frames (KB) */
uint32_t pmm_max_phys(void);      /* highest physical address seen (bytes) */