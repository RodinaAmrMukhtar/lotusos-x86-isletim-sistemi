#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
  VFS_NONE = 0,
  VFS_FAT32 = 1,
  VFS_NTFS  = 2,
} vfs_type_t;

void vfs_init(void);

/* Mount the currently-mounted FAT32 as a drive letter (C, D, ...) */
int  vfs_mount_fat32(char drive_letter, uint32_t part_lba);

/* Placeholder for next step (NTFS) */
int  vfs_mount_ntfs(char drive_letter, uint32_t part_lba);

int  vfs_is_mounted(char drive_letter);
vfs_type_t vfs_type(char drive_letter);
uint32_t vfs_part_lba(char drive_letter);
uint32_t vfs_root_cluster(char drive_letter); /* FAT32 only for now */

void vfs_list_mounts(void);

/* Helpers: parse drive prefix like "C:/path" or "D:" */
const char* vfs_strip_drive(const char* in, char current_drive, char* out_drive);
#ifdef __cplusplus
}
#endif
