#include "ui.h"
#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static const uint32_t NEON_CYAN   = 0x002CE6FFu;
static const uint32_t NEON_PURPLE = 0x00855CFFu;
static const uint32_t NEON_PINK   = 0x00FF4FD8u;

static const uint32_t BG0         = 0x0010192Eu;
static const uint32_t BG1         = 0x00152345u;

static const uint32_t TEXT        = 0x00EEF6FFu;
static const uint32_t TEXT_DIM    = 0x0092A9CCu;

static void neon_panel(int x, int y, int w, int h, int hot, int pressed) {
  uint32_t top = pressed ? 0x00121C35u : (hot ? BG1 : BG0);
  uint32_t bot = pressed ? 0x000E172Cu : 0x0010192Eu;
  uint32_t edge = hot ? NEON_CYAN : NEON_PURPLE;

  gfx_fill_round_rect_a(x + 3, y + 5, w, h, 14, 0x00050A14u, 150);
  gfx_fill_round_rect_vgrad(x, y, w, h, 14, top, bot);
  gfx_draw_round_rect(x, y, w, h, 14, edge);
  gfx_fill_rect_a(x + 10, y + 8, w - 20, 1, 0x00FFFFFFu, 28);
}

void ui_button_draw(const ui_button_t* b) {
  if (!b) return;

  neon_panel(b->x, b->y, b->w, b->h, b->hot, b->pressed);

  if (b->hot) {
    gfx_fill_round_rect_a(b->x - 2, b->y - 2, b->w + 4, b->h + 4, 16, NEON_CYAN, 28);
  }

  int fh = gfx_font_h();
  int ty = b->y + (b->h - fh) / 2;
  int tx = b->x + 14;
  gfx_draw_text(tx, ty, b->label ? b->label : "", TEXT, b->hot ? BG1 : BG0);
}

int ui_button_mouse(ui_button_t* b, int mx, int my, int l_down, int l_pressed, int l_released) {
  if (!b) return 0;

  b->hot = in_rect(mx, my, b->x, b->y, b->w, b->h);

  if (l_pressed && b->hot) {
    b->pressed = 1;
    return 1;
  }

  if (l_released) {
    int was = b->pressed;
    b->pressed = 0;
    if (was && b->hot) {
      if (b->cb) b->cb(b->userdata);
      return 1;
    }
  }

  if (!l_down) b->pressed = 0;
  return 0;
}

void ui_textbox_draw(const ui_textbox_t* t) {
  if (!t) return;

  uint32_t top = t->focused ? BG1 : BG0;
  uint32_t bot = t->focused ? 0x00111A31u : 0x000E172Cu;
  uint32_t hi  = t->focused ? NEON_CYAN : 0x00304A7Au;
  uint32_t lo  = t->focused ? NEON_PURPLE : 0x00233B60u;
  uint32_t base = t->focused ? BG1 : BG0;

  gfx_fill_round_rect_a(t->x + 3, t->y + 5, t->w, t->h, 14, 0x00050A14u, 150);
  gfx_fill_round_rect_vgrad(t->x, t->y, t->w, t->h, 14, top, bot);
  gfx_draw_round_rect(t->x, t->y, t->w, t->h, 14, hi);
  gfx_draw_round_rect(t->x + 1, t->y + 1, t->w - 2, t->h - 2, 13, lo);
  gfx_fill_rect_a(t->x + 10, t->y + 8, t->w - 20, 1, 0x00FFFFFFu, 28);

  int fh = gfx_font_h();
  int tx = t->x + 14;
  int ty = t->y + (t->h - fh) / 2;

  gfx_draw_text(tx, ty, t->text, t->focused ? TEXT : TEXT_DIM, base);

  if (t->focused) {
    int caret_x = tx + t->caret * 8;
    gfx_fill_rect(caret_x, ty, 2, fh, NEON_CYAN);
    gfx_fill_rect(caret_x + 2, ty, 1, fh, NEON_PINK);
  }
}

int ui_textbox_mouse(ui_textbox_t* t, int mx, int my, int l_pressed) {
  if (!t || !l_pressed) return 0;

  int hit = in_rect(mx, my, t->x, t->y, t->w, t->h);
  t->focused = hit;

  if (hit) {
    int rel = (mx - (t->x + 14)) / 8;
    if (rel < 0) rel = 0;
    if (rel > t->len) rel = t->len;
    t->caret = rel;
  }
  return hit;
}

int ui_textbox_char(ui_textbox_t* t, char c) {
  if (!t || !t->focused) return 0;
  if (c == '\n' || c == '\r') return 1;
  if ((unsigned char)c < 32 || (unsigned char)c > 126) return 1;
  if (t->len >= (int)sizeof(t->text) - 1) return 1;

  for (int i = t->len; i > t->caret; i--) t->text[i] = t->text[i - 1];
  t->text[t->caret] = c;
  t->len++;
  t->caret++;
  t->text[t->len] = 0;
  return 1;
}

int ui_textbox_key(ui_textbox_t* t, key_t k) {
  if (!t || !t->focused) return 0;

  if (k == KEY_LEFT)  { if (t->caret > 0) t->caret--; return 1; }
  if (k == KEY_RIGHT) { if (t->caret < t->len) t->caret++; return 1; }

  if (k == KEY_DELETE) {
    if (t->caret < t->len) {
      for (int i = t->caret; i < t->len; i++) t->text[i] = t->text[i + 1];
      t->len--;
      if (t->len < 0) t->len = 0;
      if (t->caret > t->len) t->caret = t->len;
    }
    return 1;
  }

  if (k == KEY_TAB) return 1;
  return 0;
}

#ifdef __cplusplus
}
#endif