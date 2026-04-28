#pragma once
#include <stdint.h>

// Read `count` sectors (512 bytes each) from LBA into `out`.
// Returns 0 on success, -1 on error.
int disk_read_sectors(uint32_t lba, uint8_t count, void* out);

// Write `count` sectors (512 bytes each) to LBA from `in`.
// Returns 0 on success, -1 on error.
int disk_write_sectors(uint32_t lba, uint8_t count, const void* in);