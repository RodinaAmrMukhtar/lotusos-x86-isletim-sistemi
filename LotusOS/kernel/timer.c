#include "timer.h"
#include "io.h"
#include "isr.h"
#include "events.h"
#include <stdint.h>

static volatile uint64_t g_ticks = 0;
static uint32_t g_hz = 100;

/* fixed-rate frame tick generator (60fps) */
static uint32_t g_acc60 = 0;

static void timer_irq(regs_t* r) {
  (void)r;
  g_ticks++;

  /* accumulate 60 “units” per IRQ; emit EV_TICK when >= hz */
  g_acc60 += 60;
  if (g_acc60 >= g_hz) {
    g_acc60 -= g_hz;

    event_t ev = {0};
    ev.type = EV_TICK;
    ev.ts = g_ticks;
    events_push_isr(&ev);
  }
}

void timer_init(uint32_t hz) {
  if (hz < 18) hz = 18;
  if (hz > 1000) hz = 1000;
  g_hz = hz;
  g_acc60 = 0;

  const uint32_t PIT_BASE = 1193180;
  uint32_t divisor = PIT_BASE / hz;
  if (divisor == 0) divisor = 1;
  if (divisor > 0xFFFF) divisor = 0xFFFF;

  outb(0x43, 0x36);
  outb(0x40, (uint8_t)(divisor & 0xFF));
  outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

  irq_register_handler(0, timer_irq);
}

uint64_t timer_ticks(void) { return g_ticks; }
uint32_t timer_hz(void) { return g_hz; }

uint64_t timer_uptime_ms(void) {
  return (g_ticks * 1000ULL) / (uint64_t)g_hz;
}