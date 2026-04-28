#include "terminal.h"
#include "gfx.h"
#include "timer.h"

#define TERM_W 80
#define TERM_H 25

static char g_cells[TERM_H][TERM_W];
static int cx = 0, cy = 0;

static char* cap_buf = 0;
static int   cap_max = 0;
static int   cap_len = 0;

static int g_light = 0;

static int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void term_set_theme(int light) { g_light = light ? 1 : 0; }

static void scroll_if_needed(void) {
  if (cy < TERM_H) return;

  for (int y = 1; y < TERM_H; y++)
    for (int x = 0; x < TERM_W; x++)
      g_cells[y - 1][x] = g_cells[y][x];

  for (int x = 0; x < TERM_W; x++) g_cells[TERM_H - 1][x] = ' ';
  cy = TERM_H - 1;
}

void term_clear(void) {
  for (int y = 0; y < TERM_H; y++)
    for (int x = 0; x < TERM_W; x++)
      g_cells[y][x] = ' ';
  cx = 0;
  cy = 0;
}

void term_init(const boot_info_t* bi) { (void)bi; term_clear(); }

void term_get_cursor(int* x, int* y) { if (x) *x = cx; if (y) *y = cy; }

void term_set_cursor(int x, int y) {
  cx = clampi(x, 0, TERM_W - 1);
  cy = clampi(y, 0, TERM_H - 1);
}

int term_cols(void) { return TERM_W; }
int term_rows(void) { return TERM_H; }

void term_capture_begin(char* buf, int max) {
  cap_buf = buf;
  cap_max = (max < 0) ? 0 : max;
  cap_len = 0;
  if (cap_buf && cap_max > 0) cap_buf[0] = 0;
}

int term_capture_end(void) {
  if (cap_buf && cap_max > 0) {
    if (cap_len >= cap_max) cap_len = cap_max - 1;
    if (cap_len < 0) cap_len = 0;
    cap_buf[cap_len] = 0;
  }
  cap_buf = 0;
  cap_max = 0;
  return cap_len;
}

static void cap_putc(char c) {
  if (!cap_buf || cap_max <= 0) return;
  if (cap_len >= cap_max - 1) return;
  cap_buf[cap_len++] = c;
  cap_buf[cap_len] = 0;
}

void term_putc(char c) {
  if (cap_buf) { cap_putc(c); return; }

  if (c == '\n') { cx = 0; cy++; scroll_if_needed(); return; }
  if (c == '\r') { cx = 0; return; }

  if (cx >= TERM_W) { cx = 0; cy++; scroll_if_needed(); }
  if (cy >= TERM_H) scroll_if_needed();

  g_cells[cy][cx] = c;
  cx++;
  if (cx >= TERM_W) { cx = 0; cy++; scroll_if_needed(); }
}

void term_write(const char* s) { while (s && *s) term_putc(*s++); }

/* small “syntax-ish” coloring */
static uint32_t pick_fg(char c, int is_prompt, int is_tag) {
  const uint32_t FG_D      = 0x00EEF6FFu;
  const uint32_t DIM_D     = 0x0092A9CCu;
  const uint32_t CYAN_D    = 0x002CE6FFu;
  const uint32_t PURPLE_D  = 0x00855CFFu;
  const uint32_t PINK_D    = 0x00FF4FD8u;

  const uint32_t FG_L      = 0x00111A31u;
  const uint32_t DIM_L     = 0x004A5F7Au;
  const uint32_t CYAN_L    = 0x001B7FAAu;
  const uint32_t PURPLE_L  = 0x004B3CB8u;
  const uint32_t PINK_L    = 0x00B81F6Bu;

  uint32_t FG     = g_light ? FG_L     : FG_D;
  uint32_t DIM    = g_light ? DIM_L    : DIM_D;
  uint32_t CYAN   = g_light ? CYAN_L   : CYAN_D;
  uint32_t PURPLE = g_light ? PURPLE_L : PURPLE_D;
  uint32_t PINK   = g_light ? PINK_L   : PINK_D;

  if (is_tag) return PURPLE;
  if (is_prompt) return CYAN;

  if (c >= '0' && c <= '9') return CYAN;
  if (c == '[' || c == ']' || c == '(' || c == ')' || c == '{' || c == '}' ||
      c == '<' || c == '>' || c == '/' || c == '\\' || c == '=')
    return PURPLE;

  if (c == '#' || c == ';' || c == ':' || c == ',' || c == '.')
    return DIM;

  if (c == '!' || c == '?') return PINK;
  return FG;
}

void term_render(int x_px, int y_px, int w_px, int h_px) {
  const uint32_t BG_TOP_D  = 0x00070B16u;
  const uint32_t BG_BOT_D  = 0x000B1224u;
  const uint32_t BORDER_D  = 0x00304A7Au;

  const uint32_t BG_TOP_L  = 0x00F6FAFFu;
  const uint32_t BG_BOT_L  = 0x00E8F1FFu;
  const uint32_t BORDER_L  = 0x008AA6D6u;

  const uint32_t SHINE     = 0x00FFFFFFu;

  const uint32_t CYAN_D    = 0x002CE6FFu;
  const uint32_t PURPLE_D  = 0x00855CFFu;
  const uint32_t CYAN_L    = 0x001B7FAAu;
  const uint32_t PURPLE_L  = 0x004B3CB8u;

  uint32_t BG_TOP  = g_light ? BG_TOP_L : BG_TOP_D;
  uint32_t BG_BOT  = g_light ? BG_BOT_L : BG_BOT_D;
  uint32_t BORDER  = g_light ? BORDER_L : BORDER_D;

  uint32_t CYAN    = g_light ? CYAN_L : CYAN_D;
  uint32_t PURPLE  = g_light ? PURPLE_L : PURPLE_D;

  int fh = gfx_font_h();
  int cols = w_px / 8;
  int rows = h_px / fh;
  if (cols <= 0 || rows <= 0) return;
  if (cols > TERM_W) cols = TERM_W;
  if (rows > TERM_H) rows = TERM_H;

  int tw = cols * 8;
  int th = rows * fh;

  gfx_fill_rect_vgrad(x_px, y_px, tw, th, BG_TOP, BG_BOT);
  gfx_fill_rect_a(x_px + 10, y_px + 8, tw - 20, 1, SHINE, g_light ? 20 : 18);
  gfx_fill_rect_a(x_px + 10, y_px + th - 10, tw - 20, 1, SHINE, g_light ? 14 : 10);
  gfx_draw_round_rect(x_px, y_px, tw, th, 10, BORDER);

  for (int y = 0; y < rows; y++) {
    int is_prompt = (g_cells[y][0] == '>' || g_cells[y][0] == '$');
    int is_tag = (g_cells[y][0] == '[');

    for (int x = 0; x < cols; x++) {
      char c = g_cells[y][x];
      if (!c) c = ' ';

      uint32_t fg = pick_fg(c, (is_prompt && x < 2), (is_tag && x < 6));
      gfx_draw_char(x_px + x * 8, y_px + y * fh, (uint8_t)c, fg, BG_BOT);
    }
  }

  /* caret blink */
  if (((timer_uptime_ms() / 500ULL) & 1ULL) != 0) {
    int ccx = clampi(cx, 0, cols - 1);
    int ccy = clampi(cy, 0, rows - 1);
    int px = x_px + ccx * 8;
    int py = y_px + ccy * fh;

    gfx_fill_rect_a(px - 1, py, 4, fh, CYAN, g_light ? 28 : 45);
    gfx_fill_rect(px, py, 2, fh, CYAN);
    gfx_fill_rect(px + 2, py, 1, fh, PURPLE);
  }
}

/* ---------------------------------------------------------------------------
   libc stubs (freestanding)
   GCC may emit calls to these even if you never call them directly.
--------------------------------------------------------------------------- */

typedef unsigned int size_t;

void* memset(void* dst, int c, size_t n) {
  unsigned char* p = (unsigned char*)dst;
  unsigned char v = (unsigned char)c;
  for (size_t i = 0; i < n; i++) p[i] = v;
  return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  if (d == s || n == 0) return dst;
  if (d < s) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
  } else {
    for (size_t i = n; i-- > 0; ) d[i] = s[i];
  }
  return dst;
}