#include "fat32.h"
#include "disk.h"
#include "terminal.h"
#include "kheap.h"
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

  uint32_t fat_size32;
  uint16_t ext_flags;
  uint16_t fs_version;
  uint32_t root_cluster;
  uint16_t fsinfo;
  uint16_t bkbootsec;
  uint8_t  reserved[12];
} __attribute__((packed)) bpb32_t;

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
} __attribute__((packed)) dirent_t;

typedef struct {
  uint8_t  ord;
  uint16_t name1[5];
  uint8_t  attr;      // 0x0F
  uint8_t  type;      // 0
  uint8_t  chksum;
  uint16_t name2[6];
  uint16_t fstClusLO; // 0
  uint16_t name3[2];
} __attribute__((packed)) lfn_t;
#pragma pack(pop)

#define SECTOR_SIZE 512
#define LFN_MAX 255
#define ATTR_DIR 0x10
#define ATTR_ARCH 0x20
#define ATTR_LFN 0x0F
#define ATTR_VOL 0x08

typedef struct { uint32_t lba; int idx; } dirpos_t;

static uint32_t g_lba_start = 0;
static bpb32_t  g_bpb;
static uint32_t g_fat_lba  = 0;
static uint32_t g_data_lba = 0;
static uint32_t g_total_sectors = 0;
static uint32_t g_max_cluster = 0;

/* Incremented on successful modifications so the UI can auto-refresh. */
static volatile uint32_t g_generation = 1;

static uint8_t sec[SECTOR_SIZE];
static uint8_t sec2[SECTOR_SIZE];

static int read_sector(uint32_t lba, void* out) { return disk_read_sectors(lba, 1, out); }
static int write_sector(uint32_t lba, const void* in) { return disk_write_sectors(lba, 1, in); }

static int is_space(char c) { return c == ' ' || c == '\t'; }
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int streq(const char* a, const char* b) {
  while (*a && *b) { if (*a != *b) return 0; a++; b++; }
  return *a == 0 && *b == 0;
}
static int streq_ci_ascii(const char* a, const char* b) {
  while (*a && *b) {
    char ca = up(*a++);
    char cb = up(*b++);
    if (ca != cb) return 0;
  }
  return *a == 0 && *b == 0;
}
static uint32_t first_cluster(const dirent_t* e) {
  return ((uint32_t)e->fstClusHI << 16) | (uint32_t)e->fstClusLO;
}
static void set_first_cluster(dirent_t* e, uint32_t clus) {
  e->fstClusHI = (uint16_t)((clus >> 16) & 0xFFFF);
  e->fstClusLO = (uint16_t)(clus & 0xFFFF);
}

static uint32_t cluster_lba(uint32_t clus) {
  return g_data_lba + (clus - 2) * (uint32_t)g_bpb.sectors_per_cluster;
}

static uint32_t fat_read_entry(uint32_t clus) {
  uint32_t off = clus * 4;
  uint32_t sec_index = off / SECTOR_SIZE;
  uint32_t sec_off   = off % SECTOR_SIZE;
  uint32_t lba = g_fat_lba + sec_index;

  if (read_sector(lba, sec) != 0) return 0x0FFFFFFF;

  // may straddle sector end
  if (sec_off <= SECTOR_SIZE - 4) {
    uint32_t v = (uint32_t)sec[sec_off+0] |
                 ((uint32_t)sec[sec_off+1] << 8) |
                 ((uint32_t)sec[sec_off+2] << 16) |
                 ((uint32_t)sec[sec_off+3] << 24);
    return v & 0x0FFFFFFF;
  } else {
    if (read_sector(lba + 1, sec2) != 0) return 0x0FFFFFFF;
    uint8_t tmp[4];
    for (int i = 0; i < 4; i++) {
      int p = (int)sec_off + i;
      tmp[i] = (p < (int)SECTOR_SIZE) ? sec[p] : sec2[p - (int)SECTOR_SIZE];
    }
    uint32_t v = (uint32_t)tmp[0] | ((uint32_t)tmp[1] << 8) | ((uint32_t)tmp[2] << 16) | ((uint32_t)tmp[3] << 24);
    return v & 0x0FFFFFFF;
  }
}

static int fat_write_entry(uint32_t clus, uint32_t val) {
  val &= 0x0FFFFFFF;
  uint32_t off = clus * 4;
  uint32_t sec_index = off / SECTOR_SIZE;
  uint32_t sec_off   = off % SECTOR_SIZE;

  for (uint8_t fat = 0; fat < g_bpb.num_fats; fat++) {
    uint32_t lba = g_fat_lba + sec_index + (uint32_t)fat * (uint32_t)g_bpb.fat_size32;

    if (read_sector(lba, sec) != 0) return -1;

    // handle straddle (rare): simplest safe way = write with two sectors
    if (sec_off <= SECTOR_SIZE - 4) {
      sec[sec_off+0] = (uint8_t)(val & 0xFF);
      sec[sec_off+1] = (uint8_t)((val >> 8) & 0xFF);
      sec[sec_off+2] = (uint8_t)((val >> 16) & 0xFF);
      sec[sec_off+3] = (uint8_t)((val >> 24) & 0xFF);
      if (write_sector(lba, sec) != 0) return -1;
    } else {
      if (read_sector(lba + 1, sec2) != 0) return -1;

      uint8_t bytes[4] = {
        (uint8_t)(val & 0xFF),
        (uint8_t)((val >> 8) & 0xFF),
        (uint8_t)((val >> 16) & 0xFF),
        (uint8_t)((val >> 24) & 0xFF)
      };

      for (int i = 0; i < 4; i++) {
        int p = (int)sec_off + i;
        if (p < (int)SECTOR_SIZE) sec[p] = bytes[i];
        else sec2[p - (int)SECTOR_SIZE] = bytes[i];
      }

      if (write_sector(lba, sec) != 0) return -1;
      if (write_sector(lba + 1, sec2) != 0) return -1;
    }
  }
  return 0;
}

static int zero_cluster(uint32_t clus) {
  static uint8_t z[SECTOR_SIZE];
  for (int i = 0; i < SECTOR_SIZE; i++) z[i] = 0;

  uint32_t lba0 = cluster_lba(clus);
  for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
    if (disk_write_sectors(lba0 + s, 1, z) != 0) return -1;
  }
  return 0;
}

static int alloc_cluster(uint32_t* out_clus) {
  for (uint32_t c = 2; c <= g_max_cluster; c++) {
    uint32_t v = fat_read_entry(c);
    if ((v & 0x0FFFFFFF) == 0) {
      if (fat_write_entry(c, 0x0FFFFFFF) != 0) return -1;
      if (zero_cluster(c) != 0) return -1;
      *out_clus = c;
      return 0;
    }
  }
  return -1;
}

static int free_chain(uint32_t first) {
  uint32_t cl = first;
  while (cl >= 2 && cl < 0x0FFFFFF8u) {
    uint32_t next = fat_read_entry(cl);
    if (fat_write_entry(cl, 0) != 0) return -1;
    cl = next;
  }
  return 0;
}

static void make_83(const char* in, char out_name[8], char out_ext[3]) {
  for (int i = 0; i < 8; i++) out_name[i] = ' ';
  for (int i = 0; i < 3; i++) out_ext[i] = ' ';

  while (*in && is_space(*in)) in++;

  int n = 0, e = 0, dot = 0;
  for (; *in; in++) {
    char c = *in;
    if (c == '/' || c == '\\') break;
    if (c == '"') break;
    if (c == '.') { dot = 1; continue; }
    c = up(c);
    if (!dot) { if (n < 8) out_name[n++] = c; }
    else { if (e < 3) out_ext[e++] = c; }
  }
}

static uint8_t lfn_checksum_83(const uint8_t short11[11]) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) {
    sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short11[i]);
  }
  return sum;
}

static int is_dot_component(const char* s) { return s && s[0]=='.' && s[1]==0; }
static int is_dotdot_component(const char* s) { return s && s[0]=='.' && s[1]=='.' && s[2]==0; }

static const char* next_component(const char* p, char* comp, int comp_max) {
  while (*p == '/' || *p == '\\') p++;
  int i = 0;
  while (*p && *p != '/' && *p != '\\' && i < comp_max - 1) comp[i++] = *p++;
  comp[i] = 0;
  return p;
}

// ---- LFN read buffer ----
static char lfn_buf[LFN_MAX + 1];
static int  lfn_active = 0;
static int  lfn_total  = 0;

static void lfn_reset(void) {
  lfn_active = 0;
  lfn_total = 0;
  for (int i = 0; i < LFN_MAX + 1; i++) lfn_buf[i] = 0;
}

static void lfn_feed(const lfn_t* l) {
  if (!l) return;
  if (l->attr != ATTR_LFN || l->type != 0) { lfn_reset(); return; }

  uint8_t ord = (uint8_t)(l->ord & 0x1F);
  if (ord == 0) { lfn_reset(); return; }

  if (l->ord & 0x40) {
    lfn_reset();
    lfn_active = 1;
    lfn_total = ord;
  }

  if (!lfn_active) return;
  if (ord > (uint8_t)lfn_total) { lfn_reset(); return; }

  int idx = (int)ord - 1;
  int base = idx * 13;

  uint16_t u[13];
  for (int i = 0; i < 5; i++) u[i] = l->name1[i];
  for (int i = 0; i < 6; i++) u[5+i] = l->name2[i];
  for (int i = 0; i < 2; i++) u[11+i] = l->name3[i];

  for (int j = 0; j < 13; j++) {
    int pos = base + j;
    if (pos < 0 || pos >= LFN_MAX) continue;
    uint16_t w = u[j];
    if (w == 0x0000 || w == 0xFFFF) continue;
    lfn_buf[pos] = (w < 0x80) ? (char)w : '?';
  }

  lfn_buf[LFN_MAX] = 0;
}
static int lfn_has_value(void) { return lfn_active && lfn_buf[0] != 0; }

// Find entry in directory by component (supports LFN best-effort)
static int find_in_dir(uint32_t dir_cluster, const char* comp, dirent_t* out) {
  int want_dot  = is_dot_component(comp);
  int want_dot2 = is_dotdot_component(comp);

  char want_name[8], want_ext[3];
  if (!want_dot && !want_dot2) make_83(comp, want_name, want_ext);

  lfn_reset();

  uint32_t clus = dir_cluster;
  while (clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
      if (read_sector(lba0 + s, sec) != 0) return -1;

      dirent_t* e = (dirent_t*)sec;
      for (int i = 0; i < (SECTOR_SIZE / (int)sizeof(dirent_t)); i++) {
        uint8_t first = (uint8_t)e[i].name[0];
        if (first == 0x00) return -2;
        if (first == 0xE5) { lfn_reset(); continue; }

        if (e[i].attr == ATTR_LFN) { lfn_feed((const lfn_t*)&e[i]); continue; }
        if (e[i].attr & ATTR_VOL) { lfn_reset(); continue; }

        if (lfn_has_value() && streq_ci_ascii(lfn_buf, comp)) {
          if (out) *out = e[i];
          lfn_reset();
          return 0;
        }

        if (want_dot) {
          if (e[i].name[0] == '.' && e[i].name[1] == ' ') {
            if (out) *out = e[i];
            lfn_reset();
            return 0;
          }
        }
        if (want_dot2) {
          if (e[i].name[0] == '.' && e[i].name[1] == '.') {
            if (out) *out = e[i];
            lfn_reset();
            return 0;
          }
        }

        if (!want_dot && !want_dot2) {
          int same = 1;
          for (int k = 0; k < 8; k++) if (e[i].name[k] != want_name[k]) same = 0;
          for (int k = 0; k < 3; k++) if (e[i].ext[k]  != want_ext[k])  same = 0;
          if (same) { if (out) *out = e[i]; lfn_reset(); return 0; }
        }

        lfn_reset();
      }
    }

    clus = fat_read_entry(clus);
  }

  return -2;
}

static int resolve_path(uint32_t start_cluster, const char* path, dirent_t* out, int want_dir) {
  if (!path || !path[0] || (path[0] == '/' && !path[1])) {
    if (want_dir) return 0;
    return -2;
  }

  uint32_t cur = start_cluster;
  const char* p = path;
  if (p[0] == '/' || p[0] == '\\') cur = g_bpb.root_cluster;

  char comp[128];
  while (1) {
    p = next_component(p, comp, (int)sizeof(comp));
    if (!comp[0]) break;

    const char* lookahead = p;
    while (*lookahead == '/' || *lookahead == '\\') lookahead++;

    dirent_t e;
    int r = find_in_dir(cur, comp, &e);
    if (r != 0) return -2;

    int is_dir = (e.attr & ATTR_DIR) != 0;
    uint32_t next = first_cluster(&e);
    if (next == 0) next = g_bpb.root_cluster;

    if (!*lookahead) {
      if (want_dir && !is_dir) return -2;
      if (!want_dir && is_dir) return -2;
      if (out) *out = e;
      return 0;
    } else {
      if (!is_dir) return -2;
      cur = next;
    }
  }

  return -2;
}

// ---- dir entry utilities (for write ops) ----
static void split_parent_leaf(const char* path, char* parent_out, int parent_max, char* leaf_out, int leaf_max) {
  int n = 0;
  while (path[n]) n++;

  while (n > 0 && (path[n-1] == '/' || path[n-1] == '\\')) n--;

  int last_sep = -1;
  for (int i = 0; i < n; i++) if (path[i] == '/' || path[i] == '\\') last_sep = i;

  if (last_sep < 0) {
    parent_out[0] = 0;
    int j = 0;
    for (int i = 0; i < n && j < leaf_max-1; i++) leaf_out[j++] = path[i];
    leaf_out[j] = 0;
    return;
  }

  int j = 0;
  for (int i = last_sep + 1; i < n && j < leaf_max-1; i++) leaf_out[j++] = path[i];
  leaf_out[j] = 0;

  if (last_sep == 0) {
    parent_out[0] = '/'; parent_out[1] = 0;
    return;
  }
  j = 0;
  for (int i = 0; i < last_sep && j < parent_max-1; i++) parent_out[j++] = path[i];
  parent_out[j] = 0;
}

static void short11_from_83(const char name[8], const char ext[3], uint8_t out11[11]) {
  for (int i = 0; i < 8; i++) out11[i] = (uint8_t)name[i];
  for (int i = 0; i < 3; i++) out11[8+i] = (uint8_t)ext[i];
}

static int short_exists_in_dir(uint32_t dir_cluster, const uint8_t short11[11]) {
  uint32_t clus = dir_cluster;
  while (clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);
    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
      if (read_sector(lba0 + s, sec) != 0) return 1;
      dirent_t* e = (dirent_t*)sec;
      for (int i = 0; i < (SECTOR_SIZE / (int)sizeof(dirent_t)); i++) {
        uint8_t first = (uint8_t)e[i].name[0];
        if (first == 0x00) return 0;
        if (first == 0xE5) continue;
        if (e[i].attr == ATTR_LFN) continue;
        if (e[i].attr & ATTR_VOL) continue;

        uint8_t cur11[11];
        short11_from_83(e[i].name, e[i].ext, cur11);
        int same = 1;
        for (int k = 0; k < 11; k++) if (cur11[k] != short11[k]) same = 0;
        if (same) return 1;
      }
    }
    clus = fat_read_entry(clus);
  }
  return 0;
}

static void build_short_for_long(const char* longname, uint8_t out11[11]) {
  char base[64], ext[16];
  int bi = 0, ei = 0;
  int dot = -1;

  for (int i = 0; longname[i] && i < 200; i++) if (longname[i] == '.') dot = i;

  for (int i = 0; longname[i] && i < 200; i++) {
    if (i == dot) break;
    char c = longname[i];
    if (c == '/' || c == '\\') break;
    if (c == '"') break;
    if (c == ' ') continue;
    if (c < 0x20) continue;
    if (bi < 63) base[bi++] = up(c);
  }
  base[bi] = 0;

  if (dot >= 0) {
    for (int i = dot+1; longname[i] && i < 200; i++) {
      char c = longname[i];
      if (c == '/' || c == '\\') break;
      if (c == '"') break;
      if (c == ' ') continue;
      if (c < 0x20) continue;
      if (ei < 15) ext[ei++] = up(c);
    }
  }
  ext[ei] = 0;

  char name8[8], ext3[3];
  for (int i = 0; i < 8; i++) name8[i] = ' ';
  for (int i = 0; i < 3; i++) ext3[i] = ' ';

  int nlen = 0;
  for (int i = 0; base[i] && nlen < 8; i++) name8[nlen++] = base[i];
  int elen = 0;
  for (int i = 0; ext[i] && elen < 3; i++) ext3[elen++] = ext[i];

  short11_from_83(name8, ext3, out11);
}

static void gen_unique_short(uint32_t dir_cluster, const char* longname, uint8_t out11[11]) {
  build_short_for_long(longname, out11);
  if (!short_exists_in_dir(dir_cluster, out11)) return;

  char base[64], ext[16];
  int bi = 0, ei = 0, dot = -1;
  for (int i = 0; longname[i] && i < 200; i++) if (longname[i] == '.') dot = i;

  for (int i = 0; longname[i] && i < 200; i++) {
    if (i == dot) break;
    char c = longname[i];
    if (c == ' ') continue;
    if (c == '/' || c == '\\' || c == '"') break;
    if (bi < 63) base[bi++] = up(c);
  }
  base[bi] = 0;

  if (dot >= 0) {
    for (int i = dot+1; longname[i] && i < 200; i++) {
      char c = longname[i];
      if (c == ' ') continue;
      if (c == '/' || c == '\\' || c == '"') break;
      if (ei < 15) ext[ei++] = up(c);
    }
  }
  ext[ei] = 0;

  for (int n = 1; n <= 99; n++) {
    char name8[8], ext3[3];
    for (int i = 0; i < 8; i++) name8[i] = ' ';
    for (int i = 0; i < 3; i++) ext3[i] = ' ';

    char suf1 = '~';
    char suf2 = (char)('0' + (n % 10));
    char suf0 = (n >= 10) ? (char)('0' + (n / 10)) : 0;

    int keep = 6;
    int k = 0;
    for (; base[k] && k < keep; k++) name8[k] = base[k];
    if (k < 8) name8[k++] = suf1;
    if (n >= 10 && k < 8) name8[k++] = suf0;
    if (k < 8) name8[k++] = suf2;

    int el = 0;
    for (int i = 0; ext[i] && el < 3; i++) ext3[el++] = ext[i];

    short11_from_83(name8, ext3, out11);
    if (!short_exists_in_dir(dir_cluster, out11)) return;
  }
}

static int count_lfn_entries_needed(const char* longname) {
  int len = 0;
  while (longname[len] && len < 255) len++;
  int n = (len + 12) / 13;
  if (n < 1) n = 1;
  return n;
}

static void fill_lfn_entry(lfn_t* l, int ord, int is_last, uint8_t chksum, const char* longname, int chunk_index) {
  l->ord = (uint8_t)(ord | (is_last ? 0x40 : 0x00));
  l->attr = ATTR_LFN;
  l->type = 0;
  l->chksum = chksum;
  l->fstClusLO = 0;

  for (int i = 0; i < 5; i++) l->name1[i] = 0xFFFF;
  for (int i = 0; i < 6; i++) l->name2[i] = 0xFFFF;
  for (int i = 0; i < 2; i++) l->name3[i] = 0xFFFF;

  int start = chunk_index * 13;
  for (int j = 0; j < 13; j++) {
    int pos = start + j;
    uint16_t w;
    if (pos >= 255) w = 0xFFFF;
    else if (!longname[pos]) w = 0x0000;
    else w = (uint8_t)longname[pos];
    if (j < 5) l->name1[j] = w;
    else if (j < 11) l->name2[j - 5] = w;
    else l->name3[j - 11] = w;
    if (w == 0x0000) break;
  }
}

static int extend_dir_if_needed(uint32_t dir_cluster) {
  uint32_t last = dir_cluster;
  uint32_t next = fat_read_entry(last);
  while (next >= 2 && next < 0x0FFFFFF8u) {
    last = next;
    next = fat_read_entry(last);
  }

  uint32_t newc = 0;
  if (alloc_cluster(&newc) != 0) return -1;
  if (fat_write_entry(last, newc) != 0) return -1;
  return 0;
}

static int find_free_run(uint32_t dir_cluster, int needed, dirpos_t* out_pos) {
  int run = 0;
  uint32_t clus = dir_cluster;

  while (clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
      uint32_t lba = lba0 + s;
      if (read_sector(lba, sec) != 0) return -1;

      dirent_t* e = (dirent_t*)sec;
      for (int i = 0; i < (SECTOR_SIZE / (int)sizeof(dirent_t)); i++) {
        uint8_t first = (uint8_t)e[i].name[0];
        int free = (first == 0xE5 || first == 0x00);

        if (free) {
          if (run < needed) {
            out_pos[run].lba = lba;
            out_pos[run].idx = i;
          }
          run++;
          if (run >= needed) return 0;
        } else {
          run = 0;
        }
      }
    }

    clus = fat_read_entry(clus);
  }

  if (extend_dir_if_needed(dir_cluster) != 0) return -1;
  return find_free_run(dir_cluster, needed, out_pos);
}

static int dir_write_entry_raw(uint32_t lba, int idx, const void* entry32) {
  if (read_sector(lba, sec) != 0) return -1;
  dirent_t* e = (dirent_t*)sec;
  const uint8_t* src = (const uint8_t*)entry32;
  uint8_t* dst = (uint8_t*)&e[idx];
  for (int i = 0; i < (int)sizeof(dirent_t); i++) dst[i] = src[i];
  if (write_sector(lba, sec) != 0) return -1;
  return 0;
}

static int dir_mark_deleted(uint32_t lba, int idx) {
  if (read_sector(lba, sec) != 0) return -1;
  dirent_t* e = (dirent_t*)sec;
  ((uint8_t*)(&e[idx]))[0] = 0xE5;
  if (write_sector(lba, sec) != 0) return -1;
  return 0;
}

static int find_entry_with_positions(uint32_t dir_cluster, const char* leaf, dirent_t* out,
                                     dirpos_t* lfn_pos, int* out_lfn_count,
                                     uint32_t* out_short_lba, int* out_short_idx) {
  lfn_reset();
  int lfn_count = 0;

  uint32_t clus = dir_cluster;
  while (clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
      uint32_t lba = lba0 + s;
      if (read_sector(lba, sec) != 0) return -1;

      dirent_t* e = (dirent_t*)sec;
      for (int i = 0; i < (SECTOR_SIZE / (int)sizeof(dirent_t)); i++) {
        uint8_t first = (uint8_t)e[i].name[0];
        if (first == 0x00) return -2;
        if (first == 0xE5) { lfn_reset(); lfn_count = 0; continue; }

        if (e[i].attr == ATTR_LFN) {
          if (lfn_count < 32) { lfn_pos[lfn_count].lba = lba; lfn_pos[lfn_count].idx = i; }
          if (lfn_count < 32) lfn_count++;
          lfn_feed((const lfn_t*)&e[i]);
          continue;
        }

        if (e[i].attr & ATTR_VOL) { lfn_reset(); lfn_count = 0; continue; }

        if (lfn_has_value() && streq_ci_ascii(lfn_buf, leaf)) {
          if (out) *out = e[i];
          if (out_lfn_count) *out_lfn_count = lfn_count;
          if (out_short_lba) *out_short_lba = lba;
          if (out_short_idx) *out_short_idx = i;
          lfn_reset();
          return 0;
        }

        char want_name[8], want_ext[3];
        make_83(leaf, want_name, want_ext);

        int same = 1;
        for (int k = 0; k < 8; k++) if (e[i].name[k] != want_name[k]) same = 0;
        for (int k = 0; k < 3; k++) if (e[i].ext[k]  != want_ext[k])  same = 0;

        if (same) {
          if (out) *out = e[i];
          if (out_lfn_count) *out_lfn_count = lfn_count;
          if (out_short_lba) *out_short_lba = lba;
          if (out_short_idx) *out_short_idx = i;
          lfn_reset();
          return 0;
        }

        lfn_reset();
        lfn_count = 0;
      }
    }

    clus = fat_read_entry(clus);
  }

  return -2;
}

static void print_name_83(const dirent_t* e) {
  for (int k = 0; k < 8; k++) {
    char c = e->name[k];
    if (c == ' ') break;
    term_putc(c);
  }
  if (e->ext[0] != ' ') {
    term_putc('.');
    for (int k = 0; k < 3; k++) {
      char c = e->ext[k];
      if (c == ' ') break;
      term_putc(c);
    }
  }
}

static void build_name_83(const dirent_t* e, char* out, int out_n) {
  if (!out || out_n <= 0) return;
  int j = 0;
  for (int k = 0; k < 8 && j < out_n - 1; k++) {
    char c = e->name[k];
    if (c == ' ') break;
    out[j++] = c;
  }
  if (e->ext[0] != ' ' && j < out_n - 1) {
    out[j++] = '.';
    for (int k = 0; k < 3 && j < out_n - 1; k++) {
      char c = e->ext[k];
      if (c == ' ') break;
      out[j++] = c;
    }
  }
  out[j] = 0;
}

static void print_u32(uint32_t v) {
  char buf[16];
  int i = 0;
  if (v == 0) { term_putc('0'); return; }
  while (v && i < 15) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
  while (i--) term_putc(buf[i]);
}

static int list_dir_cluster(uint32_t dir_cluster) {
  uint32_t clus = dir_cluster;
  lfn_reset();

  while (clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
      if (read_sector(lba0 + s, sec) != 0) return -1;

      dirent_t* e = (dirent_t*)sec;
      for (int i = 0; i < (SECTOR_SIZE / (int)sizeof(dirent_t)); i++) {
        uint8_t first = (uint8_t)e[i].name[0];
        if (first == 0x00) { lfn_reset(); return 0; }
        if (first == 0xE5) { lfn_reset(); continue; }

        if (e[i].attr == ATTR_LFN) { lfn_feed((const lfn_t*)&e[i]); continue; }
        if (e[i].attr & ATTR_VOL) { lfn_reset(); continue; }

        if (lfn_has_value()) term_write(lfn_buf);
        else print_name_83(&e[i]);

        if (e[i].attr & ATTR_DIR) {
          term_write("  <DIR>\n");
        } else {
          term_write("  ");
          print_u32(e[i].fileSize);
          term_write(" bytes\n");
        }

        lfn_reset();
      }
    }

    clus = fat_read_entry(clus);
  }

  lfn_reset();
  return 0;
}

int fat32_list_dir_cluster(uint32_t dir_cluster,
                           fat32_listent_t* out, int max, int* out_n) {
  if (out_n) *out_n = 0;
  if (!out || max <= 0) return -1;

  int n = 0;
  uint32_t clus = dir_cluster;
  lfn_reset();

  while (clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
      if (read_sector(lba0 + s, sec) != 0) return -1;

      dirent_t* e = (dirent_t*)sec;
      for (int i = 0; i < (SECTOR_SIZE / (int)sizeof(dirent_t)); i++) {
        uint8_t first = (uint8_t)e[i].name[0];
        if (first == 0x00) { lfn_reset(); if (out_n) *out_n = n; return 0; }
        if (first == 0xE5) { lfn_reset(); continue; }

        if (e[i].attr == ATTR_LFN) { lfn_feed((const lfn_t*)&e[i]); continue; }
        if (e[i].attr & ATTR_VOL) { lfn_reset(); continue; }

        if (n >= max) { lfn_reset(); if (out_n) *out_n = n; return 0; }

        /* name */
        out[n].name[0] = 0;
        if (lfn_has_value()) {
          int j = 0;
          while (lfn_buf[j] && j < (int)sizeof(out[n].name) - 1) {
            out[n].name[j] = lfn_buf[j];
            j++;
          }
          out[n].name[j] = 0;
        } else {
          build_name_83(&e[i], out[n].name, (int)sizeof(out[n].name));
        }

        out[n].is_dir = (uint8_t)((e[i].attr & ATTR_DIR) ? 1 : 0);
        out[n].size   = (uint32_t)((e[i].attr & ATTR_DIR) ? 0 : e[i].fileSize);

        n++;
        lfn_reset();
      }
    }

    clus = fat_read_entry(clus);
  }

  if (out_n) *out_n = n;
  return 0;
}

int fat32_mount(uint32_t lba_start) {
  g_lba_start = lba_start;

  if (read_sector(g_lba_start, sec) != 0) return -1;
  g_bpb = *(bpb32_t*)sec;

  if (g_bpb.bytes_per_sector != 512) return -1;
  if (g_bpb.sectors_per_cluster == 0) return -1;
  if (g_bpb.fat_size32 == 0) return -1;
  if (g_bpb.root_cluster < 2) return -1;

  g_total_sectors = (g_bpb.total_sectors16 != 0) ? (uint32_t)g_bpb.total_sectors16 : g_bpb.total_sectors32;

  g_fat_lba  = g_lba_start + (uint32_t)g_bpb.reserved_sectors;
  g_data_lba = g_fat_lba + (uint32_t)g_bpb.num_fats * (uint32_t)g_bpb.fat_size32;

  uint32_t data_sectors = g_total_sectors - ((uint32_t)g_bpb.reserved_sectors + (uint32_t)g_bpb.num_fats * (uint32_t)g_bpb.fat_size32);
  uint32_t cluster_count = data_sectors / (uint32_t)g_bpb.sectors_per_cluster;
  g_max_cluster = cluster_count + 1;

  return 0;
}

uint32_t fat32_root_cluster(void) { return g_bpb.root_cluster; }

uint32_t fat32_generation(void) { return (uint32_t)g_generation; }

int fat32_ls(uint32_t start_cluster, const char* path) {
  if (!path || !path[0]) return list_dir_cluster(start_cluster);
  if (path[0] == '/' && !path[1]) return list_dir_cluster(g_bpb.root_cluster);

  dirent_t e;
  uint32_t base = start_cluster;
  if (path[0] == '/' || path[0] == '\\') base = g_bpb.root_cluster;

  if (path[0] == '.' && !path[1]) return list_dir_cluster(start_cluster);

  if (resolve_path(base, path, &e, 1) != 0) {
    term_write("ls: not found\n");
    return -1;
  }

  uint32_t cl = first_cluster(&e);
  if (cl == 0) cl = g_bpb.root_cluster;
  return list_dir_cluster(cl);
}

int fat32_cat(uint32_t start_cluster, const char* path) {
  if (!path || !path[0]) { term_write("cat: missing file\n"); return -1; }

  dirent_t e;
  uint32_t base = start_cluster;
  if (path[0] == '/' || path[0] == '\\') base = g_bpb.root_cluster;

  if (resolve_path(base, path, &e, 0) != 0) {
    term_write("cat: not found\n");
    return -1;
  }

  uint32_t remaining = e.fileSize;
  uint32_t clus = first_cluster(&e);

  while (remaining > 0 && clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster && remaining > 0; s++) {
      if (read_sector(lba0 + s, sec) != 0) return -1;

      uint32_t n = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
      for (uint32_t i = 0; i < n; i++) term_putc((char)sec[i]);
      remaining -= n;
    }

    clus = fat_read_entry(clus);
  }

  term_putc('\n');
  return 0;
}

int fat32_resolve_dir(uint32_t start_cluster, const char* path, uint32_t* out_dir_cluster) {
  if (!path || !path[0] || (path[0] == '/' && !path[1])) {
    if (out_dir_cluster) {
      if (path && (path[0] == '/' || path[0] == '\\')) *out_dir_cluster = g_bpb.root_cluster;
      else *out_dir_cluster = start_cluster;
    }
    return 0;
  }

  dirent_t e;
  uint32_t base = start_cluster;
  if (path[0] == '/' || path[0] == '\\') base = g_bpb.root_cluster;

  if (resolve_path(base, path, &e, 1) != 0) return -1;

  uint32_t cl = first_cluster(&e);
  if (cl == 0) cl = g_bpb.root_cluster;
  if (out_dir_cluster) *out_dir_cluster = cl;
  return 0;
}

// ---- WRITE OPS ----

static int write_data_to_chain(uint32_t first_clus, uint32_t offset, const uint8_t* data, uint32_t size) {
  uint32_t clus = first_clus;
  uint32_t cluster_bytes = (uint32_t)g_bpb.sectors_per_cluster * SECTOR_SIZE;

  while (offset >= cluster_bytes && clus >= 2 && clus < 0x0FFFFFF8u) {
    offset -= cluster_bytes;
    clus = fat_read_entry(clus);
  }
  if (clus < 2 || clus >= 0x0FFFFFF8u) return -1;

  uint32_t data_off = 0;

  while (data_off < size && clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster && data_off < size; s++) {
      uint32_t lba = lba0 + s;

      uint32_t sector_start = (uint32_t)s * SECTOR_SIZE;
      if (offset >= sector_start + SECTOR_SIZE) continue;

      uint32_t in_sector_off = 0;
      if (offset > sector_start) in_sector_off = offset - sector_start;

      uint32_t can = SECTOR_SIZE - in_sector_off;
      uint32_t need = size - data_off;
      uint32_t n = (need < can) ? need : can;

      if (in_sector_off != 0 || n != SECTOR_SIZE) {
        if (read_sector(lba, sec) != 0) return -1;
        for (uint32_t i = 0; i < n; i++) sec[in_sector_off + i] = data[data_off + i];
        if (write_sector(lba, sec) != 0) return -1;
      } else {
        for (uint32_t i = 0; i < SECTOR_SIZE; i++) sec[i] = data[data_off + i];
        if (write_sector(lba, sec) != 0) return -1;
      }

      data_off += n;
      offset = 0;
    }

    clus = fat_read_entry(clus);
  }

  return (data_off == size) ? 0 : -1;
}

static int ensure_chain_size(uint32_t* io_first, uint32_t clusters_needed) {
  if (clusters_needed == 0) { *io_first = 0; return 0; }

  uint32_t first = *io_first;
  if (first == 0) {
    uint32_t c = 0;
    if (alloc_cluster(&c) != 0) return -1;
    first = c;
    *io_first = first;
    clusters_needed--;
  }

  uint32_t count = 1;
  uint32_t last = first;
  uint32_t next = fat_read_entry(last);
  while (next >= 2 && next < 0x0FFFFFF8u) {
    last = next;
    next = fat_read_entry(last);
    count++;
    if (count >= clusters_needed + 1) break;
  }

  while (count < clusters_needed + 1) {
    uint32_t newc = 0;
    if (alloc_cluster(&newc) != 0) return -1;
    if (fat_write_entry(last, newc) != 0) return -1;
    last = newc;
    count++;
  }

  return 0;
}

int fat32_touch(uint32_t start_cluster, const char* path) {
  if (!path || !path[0]) { term_write("touch: missing path\n"); return -1; }

  char parent[256], leaf[256];
  split_parent_leaf(path, parent, (int)sizeof(parent), leaf, (int)sizeof(leaf));
  if (!leaf[0]) { term_write("touch: bad name\n"); return -1; }

  uint32_t parent_cluster = start_cluster;
  if (path[0] == '/' || path[0] == '\\') parent_cluster = g_bpb.root_cluster;
  if (parent[0]) {
    if (fat32_resolve_dir(start_cluster, parent, &parent_cluster) != 0) {
      term_write("touch: parent not found\n");
      return -1;
    }
  }

  dirent_t existing;
  if (find_in_dir(parent_cluster, leaf, &existing) == 0) return 0;

  int nlfn = count_lfn_entries_needed(leaf);
  int needed = nlfn + 1;
  dirpos_t pos[32];
  if (needed > 32) { term_write("touch: name too long\n"); return -1; }

  if (find_free_run(parent_cluster, needed, pos) != 0) { term_write("touch: no space\n"); return -1; }

  uint8_t short11[11];
  gen_unique_short(parent_cluster, leaf, short11);
  uint8_t chksum = lfn_checksum_83(short11);

  for (int i = 0; i < nlfn; i++) {
    int ord = nlfn - i;
    int is_last = (ord == nlfn);
    int chunk_index = ord - 1;
    lfn_t l;
    fill_lfn_entry(&l, ord, is_last, chksum, leaf, chunk_index);
    if (dir_write_entry_raw(pos[i].lba, pos[i].idx, &l) != 0) return -1;
  }

  dirent_t de;
  for (int i = 0; i < (int)sizeof(de); i++) ((uint8_t*)&de)[i] = 0;
  for (int i = 0; i < 8; i++) de.name[i] = (char)short11[i];
  for (int i = 0; i < 3; i++) de.ext[i]  = (char)short11[8+i];
  de.attr = ATTR_ARCH;
  de.fileSize = 0;
  set_first_cluster(&de, 0);

  if (dir_write_entry_raw(pos[nlfn].lba, pos[nlfn].idx, &de) != 0) return -1;
  g_generation++;
  return 0;
}

int fat32_mkdir(uint32_t start_cluster, const char* path) {
  if (!path || !path[0]) { term_write("mkdir: missing path\n"); return -1; }

  char parent[256], leaf[256];
  split_parent_leaf(path, parent, (int)sizeof(parent), leaf, (int)sizeof(leaf));
  if (!leaf[0]) { term_write("mkdir: bad name\n"); return -1; }

  uint32_t parent_cluster = start_cluster;
  if (path[0] == '/' || path[0] == '\\') parent_cluster = g_bpb.root_cluster;
  if (parent[0]) {
    if (fat32_resolve_dir(start_cluster, parent, &parent_cluster) != 0) {
      term_write("mkdir: parent not found\n");
      return -1;
    }
  }

  dirent_t existing;
  if (find_in_dir(parent_cluster, leaf, &existing) == 0) {
    term_write("mkdir: already exists\n");
    return -1;
  }

  uint32_t dirclus = 0;
  if (alloc_cluster(&dirclus) != 0) { term_write("mkdir: no clusters\n"); return -1; }

  if (read_sector(cluster_lba(dirclus), sec) != 0) return -1;
  for (int i = 0; i < SECTOR_SIZE; i++) sec[i] = 0;

  dirent_t* ents = (dirent_t*)sec;
  for (int i = 0; i < (int)sizeof(dirent_t); i++) ((uint8_t*)&ents[0])[i] = 0;
  ents[0].name[0] = '.';
  for (int i = 1; i < 8; i++) ents[0].name[i] = ' ';
  for (int i = 0; i < 3; i++) ents[0].ext[i] = ' ';
  ents[0].attr = ATTR_DIR;
  set_first_cluster(&ents[0], dirclus);

  for (int i = 0; i < (int)sizeof(dirent_t); i++) ((uint8_t*)&ents[1])[i] = 0;
  ents[1].name[0] = '.';
  ents[1].name[1] = '.';
  for (int i = 2; i < 8; i++) ents[1].name[i] = ' ';
  for (int i = 0; i < 3; i++) ents[1].ext[i] = ' ';
  ents[1].attr = ATTR_DIR;
  set_first_cluster(&ents[1], parent_cluster);

  if (write_sector(cluster_lba(dirclus), sec) != 0) return -1;

  int nlfn = count_lfn_entries_needed(leaf);
  int needed = nlfn + 1;
  dirpos_t pos[32];
  if (needed > 32) { term_write("mkdir: name too long\n"); return -1; }

  if (find_free_run(parent_cluster, needed, pos) != 0) { term_write("mkdir: no space\n"); return -1; }

  uint8_t short11[11];
  gen_unique_short(parent_cluster, leaf, short11);
  uint8_t chksum = lfn_checksum_83(short11);

  for (int i = 0; i < nlfn; i++) {
    int ord = nlfn - i;
    int is_last = (ord == nlfn);
    int chunk_index = ord - 1;
    lfn_t l;
    fill_lfn_entry(&l, ord, is_last, chksum, leaf, chunk_index);
    if (dir_write_entry_raw(pos[i].lba, pos[i].idx, &l) != 0) return -1;
  }

  dirent_t de;
  for (int i = 0; i < (int)sizeof(de); i++) ((uint8_t*)&de)[i] = 0;
  for (int i = 0; i < 8; i++) de.name[i] = (char)short11[i];
  for (int i = 0; i < 3; i++) de.ext[i]  = (char)short11[8+i];
  de.attr = ATTR_DIR;
  de.fileSize = 0;
  set_first_cluster(&de, dirclus);

  if (dir_write_entry_raw(pos[nlfn].lba, pos[nlfn].idx, &de) != 0) return -1;
  g_generation++;
  return 0;
}

int fat32_write(uint32_t start_cluster, const char* path, const uint8_t* data, uint32_t size, int append) {
  if (!path || !path[0]) { term_write("write: missing path\n"); return -1; }

  char parent[256], leaf[256];
  split_parent_leaf(path, parent, (int)sizeof(parent), leaf, (int)sizeof(leaf));
  if (!leaf[0]) { term_write("write: bad name\n"); return -1; }

  uint32_t parent_cluster = start_cluster;
  if (path[0] == '/' || path[0] == '\\') parent_cluster = g_bpb.root_cluster;
  if (parent[0]) {
    if (fat32_resolve_dir(start_cluster, parent, &parent_cluster) != 0) {
      term_write("write: parent not found\n");
      return -1;
    }
  }

  dirent_t e;
  dirpos_t lfn_pos[32];
  int lfn_count = 0;
  uint32_t short_lba = 0;
  int short_idx = 0;

  int found = (find_entry_with_positions(parent_cluster, leaf, &e, lfn_pos, &lfn_count, &short_lba, &short_idx) == 0);

  if (!found) {
    if (fat32_touch(start_cluster, path) != 0) return -1;
    if (find_entry_with_positions(parent_cluster, leaf, &e, lfn_pos, &lfn_count, &short_lba, &short_idx) != 0) {
      term_write("write: internal create failed\n");
      return -1;
    }
  }

  if (e.attr & ATTR_DIR) { term_write("write: is a directory\n"); return -1; }

  uint32_t old_size = e.fileSize;
  uint32_t old_first = first_cluster(&e);

  uint32_t new_size = append ? (old_size + size) : size;

  uint32_t cluster_bytes = (uint32_t)g_bpb.sectors_per_cluster * SECTOR_SIZE;
  uint32_t need_clusters = (new_size == 0) ? 0 : ((new_size + cluster_bytes - 1) / cluster_bytes);

  uint32_t first = old_first;

  if (!append) {
    if (old_first != 0) {
      if (free_chain(old_first) != 0) return -1;
    }
    first = 0;
  }

  if (ensure_chain_size(&first, need_clusters) != 0) { term_write("write: no space\n"); return -1; }

  uint32_t off = append ? old_size : 0;
  if (size > 0 && first != 0) {
    if (write_data_to_chain(first, off, data, size) != 0) {
      term_write("write: failed\n");
      return -1;
    }
  }

  if (read_sector(short_lba, sec) != 0) return -1;
  dirent_t* ents = (dirent_t*)sec;
  dirent_t* de = &ents[short_idx];

  de->fileSize = new_size;
  set_first_cluster(de, first);

  if (write_sector(short_lba, sec) != 0) return -1;

  g_generation++;
  return 0;
}

static int dir_is_empty(uint32_t dirclus) {
  uint32_t clus = dirclus;
  lfn_reset();

  while (clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);
    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster; s++) {
      if (read_sector(lba0 + s, sec) != 0) return 0;

      dirent_t* e = (dirent_t*)sec;
      for (int i = 0; i < (SECTOR_SIZE / (int)sizeof(dirent_t)); i++) {
        uint8_t first = (uint8_t)e[i].name[0];
        if (first == 0x00) return 1;
        if (first == 0xE5) continue;
        if (e[i].attr == ATTR_LFN) continue;
        if (e[i].attr & ATTR_VOL) continue;

        if (e[i].name[0] == '.' && (e[i].name[1] == ' ' || e[i].name[1] == '.')) continue;

        return 0;
      }
    }
    clus = fat_read_entry(clus);
  }

  return 1;
}

int fat32_rm(uint32_t start_cluster, const char* path) {
  if (!path || !path[0]) { term_write("rm: missing path\n"); return -1; }

  char parent[256], leaf[256];
  split_parent_leaf(path, parent, (int)sizeof(parent), leaf, (int)sizeof(leaf));
  if (!leaf[0]) { term_write("rm: bad name\n"); return -1; }

  uint32_t parent_cluster = start_cluster;
  if (path[0] == '/' || path[0] == '\\') parent_cluster = g_bpb.root_cluster;
  if (parent[0]) {
    if (fat32_resolve_dir(start_cluster, parent, &parent_cluster) != 0) {
      term_write("rm: parent not found\n");
      return -1;
    }
  }

  dirent_t e;
  dirpos_t lfn_pos[32];
  int lfn_count = 0;
  uint32_t short_lba = 0;
  int short_idx = 0;

  if (find_entry_with_positions(parent_cluster, leaf, &e, lfn_pos, &lfn_count, &short_lba, &short_idx) != 0) {
    term_write("rm: not found\n");
    return -1;
  }

  uint32_t first = first_cluster(&e);

  if (e.attr & ATTR_DIR) {
    if (first == 0) first = g_bpb.root_cluster;
    if (!dir_is_empty(first)) {
      term_write("rm: directory not empty\n");
      return -1;
    }
  }

  if (first != 0) {
    if (free_chain(first) != 0) return -1;
  }

  for (int i = 0; i < lfn_count && i < 32; i++) {
    if (dir_mark_deleted(lfn_pos[i].lba, lfn_pos[i].idx) != 0) return -1;
  }
  if (dir_mark_deleted(short_lba, short_idx) != 0) return -1;

  g_generation++;
  return 0;
}

/* NEW: read whole file into kmalloc buffer */
int fat32_read_file(uint32_t start_cluster, const char* path,
                    uint8_t** out_buf, uint32_t* out_size) {
  if (!out_buf || !out_size) return -1;
  *out_buf = 0;
  *out_size = 0;

  if (!path || !path[0]) return -1;

  dirent_t e;
  uint32_t base = start_cluster;
  if (path[0] == '/' || path[0] == '\\') base = g_bpb.root_cluster;

  if (resolve_path(base, path, &e, 0) != 0) return -1;

  uint32_t size = e.fileSize;
  uint8_t* buf = (uint8_t*)kmalloc(size ? size : 1);
  if (!buf) return -1;

  uint32_t remaining = size;
  uint32_t clus = first_cluster(&e);
  uint32_t off = 0;

  while (remaining > 0 && clus >= 2 && clus < 0x0FFFFFF8u) {
    uint32_t lba0 = cluster_lba(clus);

    for (uint8_t s = 0; s < g_bpb.sectors_per_cluster && remaining > 0; s++) {
      if (read_sector(lba0 + s, sec) != 0) return -1;

      uint32_t n = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
      for (uint32_t i = 0; i < n; i++) buf[off + i] = sec[i];

      off += n;
      remaining -= n;
    }

    clus = fat_read_entry(clus);
  }

  *out_buf = buf;
  *out_size = size;
  return 0;
}