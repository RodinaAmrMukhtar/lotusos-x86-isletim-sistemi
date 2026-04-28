#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int fat32_mount(uint32_t lba_start);

uint32_t fat32_root_cluster(void);

int fat32_ls(uint32_t start_cluster, const char* path);
int fat32_cat(uint32_t start_cluster, const char* path);
int fat32_resolve_dir(uint32_t start_cluster, const char* path, uint32_t* out_dir_cluster);

/* write features */
int fat32_touch(uint32_t start_cluster, const char* path);
int fat32_mkdir(uint32_t start_cluster, const char* path);
int fat32_write(uint32_t start_cluster, const char* path,
                const uint8_t* data, uint32_t size, int append);
int fat32_rm(uint32_t start_cluster, const char* path);

/* NEW: read whole file into kmalloc buffer */
int fat32_read_file(uint32_t start_cluster, const char* path,
                    uint8_t** out_buf, uint32_t* out_size);

/* Directory listing for UI (fills up to max entries). */
typedef struct {
  char name[64];
  uint8_t is_dir;
  uint32_t size;
} fat32_listent_t;

int fat32_list_dir_cluster(uint32_t dir_cluster,
                           fat32_listent_t* out, int max, int* out_n);

/* Bumps whenever the FAT32 volume is modified (mkdir/touch/write/rm). */
uint32_t fat32_generation(void);

#ifdef __cplusplus
}
#endif