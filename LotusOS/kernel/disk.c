#include "disk.h"
#include "io.h"
#include <stdint.h>

#define ATA_IO       0x1F0
#define ATA_DATA     (ATA_IO + 0)
#define ATA_ERROR    (ATA_IO + 1)
#define ATA_SECCNT0  (ATA_IO + 2)
#define ATA_LBA0     (ATA_IO + 3)
#define ATA_LBA1     (ATA_IO + 4)
#define ATA_LBA2     (ATA_IO + 5)
#define ATA_HDDEVSEL (ATA_IO + 6)
#define ATA_STATUS   (ATA_IO + 7)
#define ATA_COMMAND  (ATA_IO + 7)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* -------------------- helpers -------------------- */

static inline void memcpy_dwords(void* dst, const void* src, uint32_t dwords) {
  if (!dwords) return;
  __asm__ volatile("rep movsl"
                   : "+D"(dst), "+S"(src), "+c"(dwords)
                   :
                   : "memory");
}

static inline void copy512(void* dst, const void* src) {
  memcpy_dwords(dst, src, 512u / 4u);
}

/* -------------------- PIO core -------------------- */

static int ata_wait_bsy(void) {
  for (int i = 0; i < 1000000; i++) {
    if (!(inb(ATA_STATUS) & ATA_SR_BSY)) return 0;
  }
  return -1;
}

static int ata_wait_drq(void) {
  for (int i = 0; i < 1000000; i++) {
    uint8_t s = inb(ATA_STATUS);
    if (s & ATA_SR_ERR) return -1;
    if (s & ATA_SR_DF)  return -1;
    if (s & ATA_SR_DRQ) return 0;
  }
  return -1;
}

static int ata_wait_ready(void) {
  for (int i = 0; i < 1000000; i++) {
    uint8_t s = inb(ATA_STATUS);
    if (s & ATA_SR_ERR) return -1;
    if (s & ATA_SR_DF)  return -1;
    if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRDY)) return 0;
  }
  return -1;
}

static int disk_read_sectors_pio(uint32_t lba, uint8_t count, void* out) {
  if (count == 0) return 0;

  if (ata_wait_bsy() != 0) return -1;

  outb(ATA_HDDEVSEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
  io_wait();

  outb(ATA_SECCNT0, count);
  outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
  outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
  outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
  outb(ATA_COMMAND, 0x20); // READ SECTORS (PIO)

  uint16_t* buf = (uint16_t*)out;

  for (uint8_t s = 0; s < count; s++) {
    if (ata_wait_bsy() != 0) return -1;
    if (ata_wait_drq() != 0) return -1;

    /* 256 words = 512 bytes */
    for (int i = 0; i < 256; i++) {
      buf[i] = inw(ATA_DATA);
    }
    buf += 256;
  }

  uint8_t st = inb(ATA_STATUS);
  if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
  return 0;
}

static int disk_write_sectors_pio(uint32_t lba, uint8_t count, const void* in) {
  if (count == 0) return 0;

  if (ata_wait_ready() != 0) return -1;

  outb(ATA_HDDEVSEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
  io_wait();

  outb(ATA_SECCNT0, count);
  outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
  outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
  outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
  outb(ATA_COMMAND, 0x30); // WRITE SECTORS (PIO)

  const uint16_t* buf = (const uint16_t*)in;

  for (uint8_t s = 0; s < count; s++) {
    if (ata_wait_bsy() != 0) return -1;
    if (ata_wait_drq() != 0) return -1;

    for (int i = 0; i < 256; i++) {
      outw(ATA_DATA, buf[i]);
    }
    buf += 256;
  }

  // Flush cache
  if (ata_wait_ready() != 0) return -1;
  outb(ATA_COMMAND, 0xE7); // CACHE FLUSH
  if (ata_wait_bsy() != 0) return -1;

  uint8_t st = inb(ATA_STATUS);
  if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
  return 0;
}

/* -------------------- tiny sector cache --------------------
   Direct-mapped cache for 512-byte sectors.
   This makes FAT32 directory listing + notepad much snappier because it
   repeatedly reads the FAT + directory clusters (same LBAs). */

#define DISK_CACHE_LINES 256u /* must be power-of-two */

typedef struct {
  uint32_t lba;
  uint8_t  valid;
  uint8_t  _pad[3];
  uint8_t  data[512];
} disk_cache_line_t;

static disk_cache_line_t g_cache[DISK_CACHE_LINES];

static inline disk_cache_line_t* cache_line(uint32_t lba) {
  return &g_cache[lba & (DISK_CACHE_LINES - 1u)];
}

static int disk_read_sector_cached(uint32_t lba, uint8_t* out) {
  disk_cache_line_t* line = cache_line(lba);
  if (line->valid && line->lba == lba) {
    copy512(out, line->data);
    return 0;
  }

  if (disk_read_sectors_pio(lba, 1, line->data) != 0) return -1;
  line->lba = lba;
  line->valid = 1;

  copy512(out, line->data);
  return 0;
}

static void disk_write_sector_update_cache(uint32_t lba, const uint8_t* src512) {
  disk_cache_line_t* line = cache_line(lba);
  line->lba = lba;
  line->valid = 1;
  copy512(line->data, src512);
}

/* -------------------- public API -------------------- */

int disk_read_sectors(uint32_t lba, uint8_t count, void* out) {
  if (count == 0) return 0;
  uint8_t* p = (uint8_t*)out;
  for (uint32_t i = 0; i < (uint32_t)count; i++) {
    if (disk_read_sector_cached(lba + i, p + i * 512u) != 0) return -1;
  }
  return 0;
}

int disk_write_sectors(uint32_t lba, uint8_t count, const void* in) {
  if (count == 0) return 0;

  /* write to disk first */
  if (disk_write_sectors_pio(lba, count, in) != 0) return -1;

  /* update cache so subsequent reads see the new data */
  const uint8_t* p = (const uint8_t*)in;
  for (uint32_t i = 0; i < (uint32_t)count; i++) {
    disk_write_sector_update_cache(lba + i, p + i * 512u);
  }
  return 0;
}
