#include "elf.h"
#include "terminal.h"
#include "paging.h"
#include "pmm.h"
#include <stdint.h>

#define EI_NIDENT 16

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELFCLASS32 1
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define EM_386  3
#define PT_LOAD 1

typedef struct {
  uint8_t  e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_offset;
  uint32_t p_vaddr;
  uint32_t p_paddr;
  uint32_t p_filesz;
  uint32_t p_memsz;
  uint32_t p_flags;
  uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

static void memzero(void* p, uint32_t n) {
  uint8_t* b = (uint8_t*)p;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static void memcopy(void* dst, const void* src, uint32_t n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1u); }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

int elf_load(const uint8_t* image, uint32_t size, uint32_t* out_entry) {
  if (!image || !out_entry) return -1;
  if (size < (uint32_t)sizeof(Elf32_Ehdr)) return -1;

  const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;

  if (eh->e_ident[0] != ELF_MAGIC0 ||
      eh->e_ident[1] != ELF_MAGIC1 ||
      eh->e_ident[2] != ELF_MAGIC2 ||
      eh->e_ident[3] != ELF_MAGIC3) {
    term_write("elf: bad magic\n");
    return -1;
  }

  if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB) {
    term_write("elf: unsupported class/data\n");
    return -1;
  }

  if (eh->e_type != ET_EXEC || eh->e_machine != EM_386) {
    term_write("elf: not i386 exec\n");
    return -1;
  }

  if (eh->e_phoff == 0 || eh->e_phnum == 0) {
    term_write("elf: no program headers\n");
    return -1;
  }

  if (eh->e_phentsize != sizeof(Elf32_Phdr)) {
    term_write("elf: bad phentsize\n");
    return -1;
  }

  uint32_t ph_end = eh->e_phoff + (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize;
  if (ph_end > size) {
    term_write("elf: ph out of range\n");
    return -1;
  }

  const Elf32_Phdr* ph = (const Elf32_Phdr*)(image + eh->e_phoff);

  for (uint16_t i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD) continue;

    if (ph[i].p_offset + ph[i].p_filesz > size) {
      term_write("elf: segment out of range\n");
      return -1;
    }

    if (ph[i].p_memsz < ph[i].p_filesz) {
      term_write("elf: bad memsz\n");
      return -1;
    }

    if (ph[i].p_vaddr < 0x1000u) {
      term_write("elf: vaddr too low\n");
      return -1;
    }

    uint32_t seg_start = align_down(ph[i].p_vaddr, 4096u);
    uint32_t seg_end   = align_up(ph[i].p_vaddr + ph[i].p_memsz, 4096u);

    for (uint32_t v = seg_start; v < seg_end; v += 4096u) {
      uint32_t frame = pmm_alloc_frame();
      if (!frame) {
        term_write("elf: out of frames\n");
        return -1;
      }

      /* IMPORTANT: user program pages must be USER */
      paging_map(v, frame, PAGE_RW | PAGE_USER);
      memzero((void*)(uintptr_t)v, 4096u);
    }

    memcopy((void*)(uintptr_t)ph[i].p_vaddr, image + ph[i].p_offset, ph[i].p_filesz);
  }

  *out_entry = eh->e_entry;
  return 0;
}