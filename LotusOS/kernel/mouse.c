#include "mouse.h"
#include "io.h"
#include "pic.h"
#include "isr.h"
#include "events.h"
#include "timer.h"
#include <stdint.h>

static int g_ok = 0;
static uint32_t fb_w = 0, fb_h = 0;

static int32_t mx_fp = 0, my_fp = 0; /* 16.16 */
static uint8_t buttons = 0;

static uint8_t pkt[3];
static int pkt_i = 0;

/* Tuning */
static int sens_num = 3;
static int sens_den = 2;
static int accel_threshold = 18;
static int accel_mul = 1;
static int deadzone = 0;

static inline int iabs(int x) { return x < 0 ? -x : x; }

static void ps2_wait_input(void) {
  for (int i = 0; i < 200000; i++) if ((inb(0x64) & 0x02) == 0) return;
}
static void ps2_wait_output(void) {
  for (int i = 0; i < 200000; i++) if (inb(0x64) & 0x01) return;
}
static void ps2_write_cmd(uint8_t cmd) { ps2_wait_input(); outb(0x64, cmd); }
static void ps2_write_data(uint8_t d)  { ps2_wait_input(); outb(0x60, d); }
static uint8_t ps2_read_data(void)     { ps2_wait_output(); return inb(0x60); }

static void mouse_write(uint8_t data) { ps2_write_cmd(0xD4); ps2_write_data(data); }

static void flush_ps2_output(void) {
  for (int i = 0; i < 1024; i++) {
    if (!(inb(0x64) & 0x01)) break;
    (void)inb(0x60);
  }
}

/* read ack with simple resend handling:
   returns 0xFA (ACK) on success, other value on failure */
static uint8_t mouse_cmd_ack(uint8_t cmd) {
  for (int tries = 0; tries < 8; tries++) {
    mouse_write(cmd);
    uint8_t r = ps2_read_data();
    if (r == 0xFA) return r;     /* ACK */
    if (r == 0xFE) continue;     /* RESEND */
    /* sometimes junk in buffer; try flushing and retry */
    flush_ps2_output();
  }
  return 0;
}

static void clamp_fp(void) {
  int32_t max_x = ((int32_t)fb_w - 1) << 16;
  int32_t max_y = ((int32_t)fb_h - 1) << 16;
  if (max_x < 0) max_x = 0;
  if (max_y < 0) max_y = 0;
  if (mx_fp < 0) mx_fp = 0;
  if (my_fp < 0) my_fp = 0;
  if (mx_fp > max_x) mx_fp = max_x;
  if (my_fp > max_y) my_fp = max_y;
}

static void apply_motion(int dx, int dy) {
  int ax = iabs(dx), ay = iabs(dy);

  if (ax <= deadzone) dx = 0;
  if (ay <= deadzone) dy = 0;

  dx = (dx * sens_num) / sens_den;
  dy = (dy * sens_num) / sens_den;

  if (accel_mul > 1) {
    if (ax >= accel_threshold) dx *= accel_mul;
    if (ay >= accel_threshold) dy *= accel_mul;
  }

  mx_fp += (dx << 16);
  my_fp += (dy << 16);
  clamp_fp();
}

static void mouse_irq(regs_t* r) {
  (void)r;
  if (!g_ok) return;

  uint8_t st = inb(0x64);
  if (!(st & 0x01)) return;

  /* IMPORTANT: only accept AUX bytes.
     If your mouse still doesn't move after this file, we can temporarily
     remove this check for debugging, but keep it for correctness. */
  if (!(st & 0x20)) { (void)inb(0x60); return; }

  uint8_t data = inb(0x60);

  if (pkt_i == 0) {
    if ((data & 0x08) == 0) return; /* sync */
  }

  pkt[pkt_i++] = data;
  if (pkt_i < 3) return;
  pkt_i = 0;

  uint8_t b0 = pkt[0];
  int dx = (int8_t)pkt[1];
  int dy = (int8_t)pkt[2];
  dy = -dy;

  if (b0 & 0xC0) return; /* overflow */

  uint8_t new_buttons = (uint8_t)(b0 & 0x07);
  uint8_t changed = (uint8_t)(new_buttons ^ buttons);
  buttons = new_buttons;

  apply_motion(dx, dy);

  int x = (int)(mx_fp >> 16);
  int y = (int)(my_fp >> 16);

  event_t ev = {0};
  ev.type  = EV_MOUSE;
  ev.code  = buttons;
  ev.flags = 1;
  if (changed) ev.flags |= 2;
  ev.dx = (int16_t)dx;
  ev.dy = (int16_t)dy;
  ev.x  = (int16_t)x;
  ev.y  = (int16_t)y;
  ev.ts = timer_ticks();
  events_push_isr(&ev);
}

void mouse_init(const boot_info_t* bi) {
  g_ok = 0;
  pkt_i = 0;
  buttons = 0;

  if (!bi || bi->magic != BOOTINFO_MAGIC) return;
  if (!bi->fb_width || !bi->fb_height) return;

  fb_w = bi->fb_width;
  fb_h = bi->fb_height;

  mx_fp = ((int32_t)fb_w / 2) << 16;
  my_fp = ((int32_t)fb_h / 2) << 16;
  clamp_fp();

  irq_register_handler(12, mouse_irq);

  /* enable auxiliary device */
  ps2_write_cmd(0xA8);

  /* FORCE enable IRQ1 + IRQ12 in controller command byte */
  ps2_write_cmd(0x20);
  uint8_t status = ps2_read_data();
  status |= 0x03;                 /* bit0=IRQ1, bit1=IRQ12 */
  status &= (uint8_t)~0x20;       /* clear translation bit */
  ps2_write_cmd(0x60);
  ps2_write_data(status);

  flush_ps2_output();

  /* reset mouse */
  mouse_write(0xFF);
  (void)ps2_read_data();          /* ACK / maybe resend/junk */
  (void)ps2_read_data();          /* 0xAA */
  (void)ps2_read_data();          /* id */
  flush_ps2_output();

  /* defaults + enable streaming (retry-safe) */
  (void)mouse_cmd_ack(0xF6);
  uint8_t ok = mouse_cmd_ack(0xF4);

  /* unmask cascade + IRQ12 */
  pic_clear_mask(2);
  pic_clear_mask(12);

  /* even if ack was flaky, try anyway */
  g_ok = (ok == 0xFA) ? 1 : 1;
}

void mouse_get_pos(int* x, int* y) {
  if (x) *x = (int)(mx_fp >> 16);
  if (y) *y = (int)(my_fp >> 16);
}