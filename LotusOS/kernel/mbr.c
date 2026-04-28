#include "mbr.h"
#include "disk.h"
#include <stdint.h>

#define SECTOR_SIZE 512
static uint8_t sec[SECTOR_SIZE];

int mbr_read_partitions(mbr_part_t out_parts[4]) {
  if (disk_read_sectors(0, 1, sec) != 0) return -1;

  // MBR signature
  if (sec[510] != 0x55 || sec[511] != 0xAA) return -1;

  /* If this is actually a FAT boot sector ("superfloppy"), not an MBR,
     then it contains a filesystem type string.
     FAT12/16 place it at 0x36, FAT32 at 0x52. */
  if ((sec[0x52] == 'F' && sec[0x53] == 'A' && sec[0x54] == 'T' && sec[0x55] == '3' && sec[0x56] == '2') ||
      (sec[0x36] == 'F' && sec[0x37] == 'A' && sec[0x38] == 'T')) {
    return -1;
  }

  // Partition table at offset 446
  const mbr_part_t* p = (const mbr_part_t*)(sec + 446);
  for (int i = 0; i < 4; i++) out_parts[i] = p[i];
  return 0;
}

int mbr_find_first_fat32(uint32_t* out_lba_start, uint32_t* out_sectors) {
  mbr_part_t p[4];
  if (mbr_read_partitions(p) != 0) return -1;

  for (int i = 0; i < 4; i++) {
    uint8_t t = p[i].type;
    // FAT32 types: 0x0B (CHS), 0x0C (LBA)
    if ((t == 0x0B || t == 0x0C) && p[i].lba_start != 0 && p[i].sectors != 0) {
      if (out_lba_start) *out_lba_start = p[i].lba_start;
      if (out_sectors)   *out_sectors   = p[i].sectors;
      return 0;
    }
  }
  return -1;
}