#pragma once
#include <stdint.h>

typedef struct {
  uint8_t  status;
  uint8_t  chs_first[3];
  uint8_t  type;
  uint8_t  chs_last[3];
  uint32_t lba_start;
  uint32_t sectors;
} __attribute__((packed)) mbr_part_t;

int mbr_read_partitions(mbr_part_t out_parts[4]);
int mbr_find_first_fat32(uint32_t* out_lba_start, uint32_t* out_sectors);