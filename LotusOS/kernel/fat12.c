#include "fat12.h"
#include "disk.h"
#include "terminal.h"
#include <stdint.h>
#include <stddef.h>

#pragma pack(push, 1)
typedef struct {
  uint8_t  jmp[3];
  uint8_t  oem[8];
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  num_fats;
  uint16_t root_entry_count;
  uint16_t total_sectors16;
  uint8_t  media;
  uint16_t fat_size16;
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors32;
  // extended fields exist but we don’t need them for FAT12 read-only
} bpb_t;

typedef struct {
  char     name[8];
  char     ext[3];
  uint8_t  attr;
  uint8_t  ntres;
  uint8_t  crtTimeTenth;
  uint16_t crtTime;
  uint16_t crtDate;
  uint16_t lstAccDate;
  uint16_t fstClusHI;
  uint16_t wrtTime;
  uint16_t wrtDate;
  uint16_t fstClusLO;
  uint32_t fileSize;
} dirent_t;
#pragma pack(pop)

static uint32_t g_fs_lba = 0;
static bpb_t g_bpb;

static uint32_t g_fat_lba = 0;
static uint32_t g_root_lba = 0;
static uint32_t g_root_sectors = 0;
static uint32_t g_data_lba = 0;

#define SECTOR_SIZE 512
static uint8_t sector[SECTOR_SIZE];

// FAT buffer (enough for small FAT12 volumes we’ll use)
#define FAT_MAX_SECTORS 128
static uint8_t fatbuf[FAT_MAX_SECTORS * SECTOR_SIZE];

static void write_u32(uint32_t v) {
  char buf[16];
  int i = 0;
  if (v == 0) { term_putc('0'); return; }
  while (v && i < 15) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i--) term_putc(buf[i]);
}

static int is_space(char c) { return c == ' ' || c == '\t'; }

static char up(char c) {
  if (c >= 'a' && c <= 'z') return (char)(c - 32);
  return c;
}

static void make_83(const char* in, char out_name[8], char out_ext[3]) {
  for (int i = 0; i < 8; i++) out_name[i] = ' ';
  for (int i = 0; i < 3; i++) out_ext[i] = ' ';

  int n = 0, e = 0;
  int seen_dot = 0;

  while (*in && is_space(*in)) in++;

  for (; *in; in++) {
    char c = *in;
    if (c == '.') { seen_dot = 1; continue; }
    c = up(c);
    if (!seen_dot) {
      if (n < 8) out_name[n++] = c;
    } else {
      if (e < 3) out_ext[e++] = c;
    }
  }
}

static uint16_t fat12_next_cluster(uint16_t clus) {
  // FAT12: 12-bit entries
  uint32_t off = (uint32_t)clus + ((uint32_t)clus / 2);
  uint16_t v = (uint16_t)fatbuf[off] | ((uint16_t)fatbuf[off + 1] << 8);

  if (clus & 1) v >>= 4;
  else v &= 0x0FFF;

  return v;
}

static uint32_t cluster_lba(uint16_t clus) {
  // cluster numbers start at 2
  return g_data_lba + (uint32_t)(clus - 2) * (uint32_t)g_bpb.sectors_per_cluster;
}

int fat12_mount(uint32_t fs_lba_start) {
  g_fs_lba = fs_lba_start;

  if (disk_read_sectors(g_fs_lba, 1, sector) != 0) return -1;

  g_bpb = *(bpb_t*)sector;
  if (g_bpb.bytes_per_sector != 512) return -1;
  if (g_bpb.fat_size16 == 0) return -1;

  g_fat_lba = g_fs_lba + g_bpb.reserved_sectors;

  g_root_sectors = ((g_bpb.root_entry_count * 32U) + (g_bpb.bytes_per_sector - 1)) / g_bpb.bytes_per_sector;
  g_root_lba = g_fat_lba + (uint32_t)g_bpb.num_fats * (uint32_t)g_bpb.fat_size16;

  g_data_lba = g_root_lba + g_root_sectors;

  if (g_bpb.fat_size16 > FAT_MAX_SECTORS) return -1;

  // read FAT#1 into memory
  if (disk_read_sectors(g_fat_lba, (uint8_t)g_bpb.fat_size16, fatbuf) != 0) return -1;

  return 0;
}

void fat12_ls_root(void) {
  term_write("Root directory:\n");

  for (uint32_t s = 0; s < g_root_sectors; s++) {
    if (disk_read_sectors(g_root_lba + s, 1, sector) != 0) {
      term_write("ls: disk error\n");
      return;
    }

    dirent_t* e = (dirent_t*)sector;
    for (int i = 0; i < (int)(SECTOR_SIZE / sizeof(dirent_t)); i++) {
      uint8_t first = (uint8_t)e[i].name[0];
      if (first == 0x00) return;          // end
      if (first == 0xE5) continue;        // deleted
      if (e[i].attr == 0x0F) continue;    // LFN
      if (e[i].attr & 0x08) continue;     // volume label

      // print NAME.EXT
      for (int k = 0; k < 8; k++) {
        char c = e[i].name[k];
        if (c == ' ') break;
        term_putc(c);
      }
      if (e[i].ext[0] != ' ') {
        term_putc('.');
        for (int k = 0; k < 3; k++) {
          char c = e[i].ext[k];
          if (c == ' ') break;
          term_putc(c);
        }
      }

      term_write("  ");
      write_u32(e[i].fileSize);
      term_write(" bytes\n");
    }
  }
}

int fat12_cat(const char* name83) {
  char want_name[8], want_ext[3];
  make_83(name83, want_name, want_ext);

  dirent_t found;
  int ok = 0;

  for (uint32_t s = 0; s < g_root_sectors && !ok; s++) {
    if (disk_read_sectors(g_root_lba + s, 1, sector) != 0) return -1;

    dirent_t* e = (dirent_t*)sector;
    for (int i = 0; i < (int)(SECTOR_SIZE / sizeof(dirent_t)); i++) {
      uint8_t first = (uint8_t)e[i].name[0];
      if (first == 0x00) break;
      if (first == 0xE5) continue;
      if (e[i].attr == 0x0F) continue;
      if (e[i].attr & 0x08) continue;

      int same = 1;
      for (int k = 0; k < 8; k++) if (e[i].name[k] != want_name[k]) same = 0;
      for (int k = 0; k < 3; k++) if (e[i].ext[k] != want_ext[k]) same = 0;

      if (same) {
        found = e[i];
        ok = 1;
        break;
      }
    }
  }

  if (!ok) {
    term_write("cat: not found\n");
    return -1;
  }

  uint32_t remaining = found.fileSize;
  uint16_t clus = found.fstClusLO;

  while (remaining > 0 && clus >= 2 && clus < 0xFF8) {
    uint32_t lba = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster && remaining > 0; s++) {
      if (disk_read_sectors(lba + s, 1, sector) != 0) return -1;

      uint32_t n = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
      for (uint32_t i = 0; i < n; i++) term_putc((char)sector[i]);

      remaining -= n;
    }

    clus = fat12_next_cluster(clus);
  }

  term_putc('\n');
  return 0;
}