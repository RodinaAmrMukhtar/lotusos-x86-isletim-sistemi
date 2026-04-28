#include "vfs.h"
#include "terminal.h"
#include "fat32.h"
#include <stdint.h>

typedef struct {
  char drive;            /* 'C', 'D', ... */
  vfs_type_t type;
  uint32_t part_lba;

  /* For FAT32 we keep root cluster. For NTFS later we'll store NTFS context. */
  uint32_t root_cluster;
} vfs_mount_t;

static vfs_mount_t g_mounts[8];

static char up(char c) {
  if (c >= 'a' && c <= 'z') return (char)(c - 32);
  return c;
}

static void write_u32(uint32_t n) {
  char buf[16];
  int i = 0;
  if (n == 0) { term_putc('0'); return; }
  while (n && i < 15) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
  while (i--) term_putc(buf[i]);
}

void vfs_init(void) {
  for (int i = 0; i < 8; i++) {
    g_mounts[i].drive = 0;
    g_mounts[i].type = VFS_NONE;
    g_mounts[i].part_lba = 0;
    g_mounts[i].root_cluster = 0;
  }
}

static vfs_mount_t* find_mount(char drive) {
  drive = up(drive);
  for (int i = 0; i < 8; i++) {
    if (g_mounts[i].type != VFS_NONE && g_mounts[i].drive == drive) return &g_mounts[i];
  }
  return 0;
}

static vfs_mount_t* alloc_mount_slot(void) {
  for (int i = 0; i < 8; i++) {
    if (g_mounts[i].type == VFS_NONE) return &g_mounts[i];
  }
  return 0;
}

int vfs_is_mounted(char drive_letter) {
  return find_mount(drive_letter) != 0;
}

vfs_type_t vfs_type(char drive_letter) {
  vfs_mount_t* m = find_mount(drive_letter);
  return m ? m->type : VFS_NONE;
}

uint32_t vfs_part_lba(char drive_letter) {
  vfs_mount_t* m = find_mount(drive_letter);
  return m ? m->part_lba : 0;
}

uint32_t vfs_root_cluster(char drive_letter) {
  vfs_mount_t* m = find_mount(drive_letter);
  return m ? m->root_cluster : 0;
}

int vfs_mount_fat32(char drive_letter, uint32_t part_lba) {
  drive_letter = up(drive_letter);
  if (drive_letter < 'A' || drive_letter > 'Z') return -1;

  vfs_mount_t* m = find_mount(drive_letter);
  if (!m) m = alloc_mount_slot();
  if (!m) return -1;

  m->drive = drive_letter;
  m->type = VFS_FAT32;
  m->part_lba = part_lba;
  m->root_cluster = fat32_root_cluster();
  return 0;
}

int vfs_mount_ntfs(char drive_letter, uint32_t part_lba) {
  (void)drive_letter;
  (void)part_lba;
  /* Next step: real NTFS driver */
  return -1;
}

void vfs_list_mounts(void) {
  term_write("Mounted volumes:\n");
  int any = 0;
  for (int i = 0; i < 8; i++) {
    if (g_mounts[i].type == VFS_NONE) continue;
    any = 1;

    term_write("  ");
    term_putc(g_mounts[i].drive);
    term_write(":  ");

    if (g_mounts[i].type == VFS_FAT32) term_write("FAT32");
    else if (g_mounts[i].type == VFS_NTFS) term_write("NTFS");
    else term_write("UNKNOWN");

    term_write("  (LBA=");
    write_u32(g_mounts[i].part_lba);
    term_write(")\n");
  }
  if (!any) term_write("  (none)\n");
}

const char* vfs_strip_drive(const char* in, char current_drive, char* out_drive) {
  if (!in || !in[0]) {
    *out_drive = up(current_drive);
    return in;
  }

  /* Pattern: X:... */
  if (in[1] == ':') {
    char d = up(in[0]);
    *out_drive = d;
    return in + 2; /* can be "", "/", "path" */
  }

  *out_drive = up(current_drive);
  return in;
}