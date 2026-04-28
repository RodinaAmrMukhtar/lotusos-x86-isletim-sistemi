#include "events.h"
#include <stdint.h>

#define EVQ_SIZE 1024u

static volatile uint32_t g_head = 0;
static volatile uint32_t g_tail = 0;
static event_t g_q[EVQ_SIZE];

static inline uint32_t irq_save_cli(void) {
  uint32_t f;
  __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
  return f;
}

static inline void irq_restore(uint32_t f) {
  if (f & (1u << 9)) __asm__ volatile("sti" ::: "memory");
}

static inline uint32_t q_prev(uint32_t idx) {
  return (idx - 1u) & (EVQ_SIZE - 1u);
}

void events_init(void) {
  uint32_t f = irq_save_cli();
  g_head = 0;
  g_tail = 0;
  irq_restore(f);
}

void events_push_isr(const event_t* ev) {
  if (!ev) return;

  uint32_t head = g_head;
  uint32_t next = (head + 1u) & (EVQ_SIZE - 1u);

  /* Coalesce mouse move spam when buttons did not change. */
  if (ev->type == EV_MOUSE && (ev->flags & 2u) == 0u && head != g_tail) {
    uint32_t prev = q_prev(head);
    if (g_q[prev].type == EV_MOUSE && (g_q[prev].flags & 2u) == 0u) {
      g_q[prev] = *ev;
      return;
    }
  }

  /* Coalesce ticks so we never backlog “frames”. */
  if (ev->type == EV_TICK && head != g_tail) {
    uint32_t prev = q_prev(head);
    if (g_q[prev].type == EV_TICK) {
      g_q[prev] = *ev;
      return;
    }
  }

  if (next == g_tail) return; /* full -> drop newest */

  g_q[head] = *ev;
  g_head = next;
}

int events_pop(event_t* out) {
  uint32_t f = irq_save_cli();

  if (g_tail == g_head) {
    irq_restore(f);
    return 0;
  }

  *out = g_q[g_tail];
  g_tail = (g_tail + 1u) & (EVQ_SIZE - 1u);

  irq_restore(f);
  return 1;
}

void events_wait(event_t* out) {
  while (!events_pop(out)) {
    __asm__ volatile("hlt");
  }
}