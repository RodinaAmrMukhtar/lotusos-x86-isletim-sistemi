#include "kheap.h"
#include <stdint.h>
#include <stddef.h>

static uint32_t g_heap_start = 0;
static uint32_t g_heap_cur   = 0;
static uint32_t g_heap_end   = 0;

static uint32_t align_up(uint32_t v, uint32_t a) {
  return (v + a - 1u) & ~(a - 1u);
}

void kheap_init(uint32_t heap_start, uint32_t heap_end) {
  g_heap_start = heap_start;
  g_heap_cur   = heap_start;
  g_heap_end   = heap_end;
}

void* kmalloc(size_t size) {
  if (size == 0) return (void*)0;
  uint32_t n = (uint32_t)size;
  n = align_up(n, 8u);

  uint32_t cur = align_up(g_heap_cur, 8u);
  if (cur + n < cur) return (void*)0;
  if (cur + n > g_heap_end) return (void*)0;

  g_heap_cur = cur + n;
  return (void*)(uintptr_t)cur;
}

void* kmalloc_aligned(size_t size, uint32_t align) {
  if (align == 0) align = 8;
  uint32_t cur = align_up(g_heap_cur, align);
  uint32_t n = (uint32_t)size;
  if (cur + n < cur) return (void*)0;
  if (cur + n > g_heap_end) return (void*)0;
  g_heap_cur = cur + n;
  return (void*)(uintptr_t)cur;
}

uint32_t kheap_total_bytes(void) {
  return (g_heap_end >= g_heap_start) ? (g_heap_end - g_heap_start) : 0;
}
uint32_t kheap_used_bytes(void) {
  return (g_heap_cur >= g_heap_start) ? (g_heap_cur - g_heap_start) : 0;
}
uint32_t kheap_free_bytes(void) {
  uint32_t t = kheap_total_bytes();
  uint32_t u = kheap_used_bytes();
  return (t >= u) ? (t - u) : 0;
}