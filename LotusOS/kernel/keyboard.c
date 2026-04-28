#include "keyboard.h"
#include "isr.h"
#include "io.h"
#include "keys.h"
#include "events.h"
#include "timer.h"
#include <stdint.h>

static int shift_l = 0;
static int shift_r = 0;
static int ctrl_l  = 0;
static int ctrl_r  = 0;
static int alt_l   = 0;
static int alt_r   = 0;
static int caps_lock = 0;
static int e0_prefix = 0;

static inline int shift_down(void) { return shift_l || shift_r; }

/* base map (no shift) */
static const char keymap[128] = {
  [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
  [0x0C]='-',[0x0D]='=',
  [0x0E]='\b',
  [0x0F]='\t',
  [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
  [0x1A]='[',[0x1B]=']',
  [0x1C]='\n',
  [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
  [0x27]=';',[0x28]='\'',[0x29]='`',
  [0x2B]='\\',
  [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',
  [0x33]=',',[0x34]='.',[0x35]='/',
  [0x39]=' ',
};

/* shift map (numbers/punct only; letters handled by shift^caps) */
static const char keymap_shift[128] = {
  [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
  [0x0C]='_',[0x0D]='+',
  [0x1A]='{',[0x1B]='}',
  [0x27]=':',[0x28]='"',[0x29]='~',
  [0x2B]='|',
  [0x33]='<',[0x34]='>',[0x35]='?',
};

static uint8_t cur_mods(void) {
  uint8_t m = 0;
  if (shift_down()) m |= MOD_SHIFT;
  if (ctrl_l || ctrl_r) m |= MOD_CTRL;
  if (alt_l || alt_r)   m |= MOD_ALT;
  if (caps_lock)    m |= MOD_CAPS;
  return m;
}

static void push_char(char c) {
  event_t ev = {0};
  ev.type = EV_CHAR;
  ev.code = (uint8_t)c;
  ev.mods = cur_mods();
  ev.ts   = timer_ticks();
  events_push_isr(&ev);
}

static void push_key(key_t k) {
  event_t ev = {0};
  ev.type  = EV_KEY;
  ev.code  = (uint8_t)k;
  ev.flags = 1; /* pressed */
  ev.mods  = cur_mods();
  ev.ts    = timer_ticks();
  events_push_isr(&ev);
}

/* ----------------- key repeat ----------------- */
#define REPEAT_DELAY_MS 60
#define REPEAT_RATE_MS 8

static uint16_t held_id = 0;     /* 0x00XX or 0xE0XX */
static uint8_t  held_kind = 0;   /* 0 none, 1 char, 2 key, 3 backspace-pair */
static uint8_t  held_code = 0;   /* char or key */
static uint64_t held_next_ms = 0;
static int      bs_phase = 0;    /* for backspace pair */

static void repeat_clear(void) {
  held_id = 0;
  held_kind = 0;
  held_code = 0;
  held_next_ms = 0;
  bs_phase = 0;
}

static void repeat_set(uint16_t id, uint8_t kind, uint8_t code) {
  held_id = id;
  held_kind = kind;
  held_code = code;
  held_next_ms = timer_uptime_ms() + REPEAT_DELAY_MS;
  bs_phase = 0;
}

/* called by kernel on EV_TICK to get a repeat event (processed immediately) */
int keyboard_repeat_event(event_t* out) {
  if (!out) return 0;
  if (!held_id || !held_kind) return 0;

  uint64_t now = timer_uptime_ms();
  if (now < held_next_ms) return 0;

  /* emit at most one per call */
  if (held_kind == 1) {
    out->type = EV_CHAR;
    out->code = held_code;
    out->flags = 0;
    out->mods = cur_mods();
    out->dx = out->dy = 0;
    out->x = out->y = 0;
    out->ts = timer_ticks();
    held_next_ms += REPEAT_RATE_MS;
    return 1;
  }

  if (held_kind == 2) {
    out->type = EV_KEY;
    out->code = held_code;
    out->flags = 1;
    out->mods = cur_mods();
    out->dx = out->dy = 0;
    out->x = out->y = 0;
    out->ts = timer_ticks();
    held_next_ms += REPEAT_RATE_MS;
    return 1;
  }

  /* backspace pair: LEFT then DELETE */
  if (held_kind == 3) {
    out->type = EV_KEY;
    out->flags = 1;
    out->mods = cur_mods();
    out->dx = out->dy = 0;
    out->x = out->y = 0;
    out->ts = timer_ticks();

    if (bs_phase == 0) {
      out->code = (uint8_t)KEY_LEFT;
      bs_phase = 1;
      return 1;
    } else {
      out->code = (uint8_t)KEY_DELETE;
      bs_phase = 0;
      held_next_ms += REPEAT_RATE_MS;
      return 1;
    }
  }

  return 0;
}
/* --------------------------------------------- */

static void keyboard_irq(regs_t* r) {
  (void)r;
  uint8_t sc = inb(0x60);

  if (sc == 0xE0) { e0_prefix = 1; return; }

  int release = (sc & 0x80) != 0;
  uint8_t code = sc & 0x7F;
  uint16_t id = (uint16_t)(e0_prefix ? (0xE000u | code) : code);
  e0_prefix = 0;

  if (release) {
    if (code == 0x2A) shift_l = 0;
    if (code == 0x36) shift_r = 0;
    if (id == 0x001Du) ctrl_l = 0;
    if (id == 0xE01Du) ctrl_r = 0;
    if (id == 0x0038u) alt_l = 0;
    if (id == 0xE038u) alt_r = 0;
    if (held_id == id) repeat_clear();
    return;
  }

  /* modifiers */
  if (code == 0x2A) { shift_l = 1; return; }
  if (code == 0x36) { shift_r = 1; return; }
  if (id == 0x001Du) { ctrl_l = 1; return; }
  if (id == 0xE01Du) { ctrl_r = 1; return; }
  if (id == 0x0038u) { alt_l = 1; return; }
  if (id == 0xE038u) { alt_r = 1; return; }
  if (code == 0x3A) { caps_lock = !caps_lock; return; }

  /* extended arrows/delete */
  if ((id & 0xFF00u) == 0xE000u) {
    switch (code) {
      case 0x48: push_key(KEY_UP);     repeat_set(id, 2, (uint8_t)KEY_UP);     return;
      case 0x50: push_key(KEY_DOWN);   repeat_set(id, 2, (uint8_t)KEY_DOWN);   return;
      case 0x4B: push_key(KEY_LEFT);   repeat_set(id, 2, (uint8_t)KEY_LEFT);   return;
      case 0x4D: push_key(KEY_RIGHT);  repeat_set(id, 2, (uint8_t)KEY_RIGHT);  return;
      case 0x53: push_key(KEY_DELETE); repeat_set(id, 2, (uint8_t)KEY_DELETE); return;
      default: return;
    }
  }

  char c = keymap[code];
  if (!c) return;

  if (c == '\t') { push_key(KEY_TAB); repeat_set(id, 2, (uint8_t)KEY_TAB); return; }

  if (c == '\b') {
    push_key(KEY_LEFT);
    push_key(KEY_DELETE);
    repeat_set(id, 3, 0);
    return;
  }

  if (c == '\n') { push_char('\n'); repeat_set(id, 1, (uint8_t)'\n'); return; }

  if (c >= 'a' && c <= 'z') {
    if (shift_down() ^ caps_lock) c = (char)(c - 32);
  } else {
    if (shift_down() && keymap_shift[code]) c = keymap_shift[code];
  }

  if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
    push_char(c);
    repeat_set(id, 1, (uint8_t)c);
  }
}

void keyboard_init(void) {
  repeat_clear();
  irq_register_handler(1, keyboard_irq);
}