#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void kheap_init(uint32_t heap_start, uint32_t heap_end);
void* kmalloc(size_t size);
void* kmalloc_aligned(size_t size, uint32_t align);

/* Stats */
uint32_t kheap_total_bytes(void);
uint32_t kheap_used_bytes(void);
uint32_t kheap_free_bytes(void);

#ifdef __cplusplus
}
#endif