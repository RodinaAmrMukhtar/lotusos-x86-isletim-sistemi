#pragma once
#include <stdint.h>

typedef enum {
  EV_NONE  = 0,
  EV_KEY   = 1,   /* special keys: arrows, delete, tab */
  EV_CHAR  = 2,   /* printable chars + '\n' */
  EV_MOUSE = 3,   /* mouse move/buttons */
  EV_TICK  = 4    /* frame tick (60fps) */
} event_type_t;

/* modifiers (mods field) */
#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_CAPS  0x08

typedef struct __attribute__((packed)) event {
  uint8_t  type;
  uint8_t  code;
  uint8_t  flags;
  uint8_t  mods;

  int16_t  dx, dy;
  int16_t  x, y;

  uint64_t ts;       /* timer ticks when event created */
} event_t;

void events_init(void);
void events_push_isr(const event_t* ev);
int  events_pop(event_t* out);
void events_wait(event_t* out);