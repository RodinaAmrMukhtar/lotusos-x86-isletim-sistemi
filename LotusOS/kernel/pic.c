#include "pic.h"
#include "io.h"

#define PIC1 0x20
#define PIC2 0xA0
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1 + 1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2 + 1)

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

void pic_remap(uint8_t offset1, uint8_t offset2) {
  uint8_t a1 = inb(PIC1_DATA);
  uint8_t a2 = inb(PIC2_DATA);

  outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
  io_wait();
  outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
  io_wait();

  outb(PIC1_DATA, offset1);
  io_wait();
  outb(PIC2_DATA, offset2);
  io_wait();

  outb(PIC1_DATA, 4);   // PIC2 is connected to IRQ2
  io_wait();
  outb(PIC2_DATA, 2);
  io_wait();

  outb(PIC1_DATA, ICW4_8086);
  io_wait();
  outb(PIC2_DATA, ICW4_8086);
  io_wait();

  outb(PIC1_DATA, a1);
  outb(PIC2_DATA, a2);
}

void pic_set_masks(uint8_t master_mask, uint8_t slave_mask) {
  outb(PIC1_DATA, master_mask);
  outb(PIC2_DATA, slave_mask);
}

uint16_t pic_get_masks(void) {
  uint8_t m = inb(PIC1_DATA);
  uint8_t s = inb(PIC2_DATA);
  return (uint16_t)m | ((uint16_t)s << 8);
}

void pic_set_mask(uint8_t irq) {
  uint16_t port;
  uint8_t  value;

  if (irq < 8) {
    port = PIC1_DATA;
  } else {
    port = PIC2_DATA;
    irq -= 8;
  }

  value = inb((uint16_t)port) | (uint8_t)(1u << irq);
  outb((uint16_t)port, value);
}

void pic_clear_mask(uint8_t irq) {
  uint16_t port;
  uint8_t  value;

  if (irq < 8) {
    port = PIC1_DATA;
  } else {
    port = PIC2_DATA;
    irq -= 8;
  }

  value = inb((uint16_t)port) & (uint8_t)~(1u << irq);
  outb((uint16_t)port, value);
}

void pic_send_eoi(uint8_t irq) {
  if (irq >= 8) outb(PIC2_COMMAND, 0x20);
  outb(PIC1_COMMAND, 0x20);
}