#include "splash.h"
#include "gfx.h"
#include "events.h"
#include "timer.h"
#include "logo.h"
#include "kheap.h"
#include <stdint.h>

static int should_skip_on_event(const event_t* ev) {
  if (!ev) return 0;
  if (ev->type == EV_CHAR || ev->type == EV_KEY) return 1;
  /* EV_MOUSE: skip only on button change, not movement */
  if (ev->type == EV_MOUSE && (ev->flags & 2)) return 1;
  return 0;
}

static inline uint32_t rgb_u8(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Convert embedded RGBA bytes to ARGB32 once, then blit with alpha blending. */
static const uint32_t* get_logo_argb(void) {
  static uint32_t* cached = 0;
  if (cached) return cached;

  const int w = LEIURUS_LOGO_W;
  const int h = LEIURUS_LOGO_H;
  uint32_t* buf = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4u);
  if (!buf) return 0;

  const uint8_t* rgba = (const uint8_t*)leiurus_logo_rgba;
  for (int i = 0; i < w * h; i++) {
    uint8_t r = rgba[i * 4 + 0];
    uint8_t g = rgba[i * 4 + 1];
    uint8_t b = rgba[i * 4 + 2];
    uint8_t a = rgba[i * 4 + 3];
    buf[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
  }

  cached = buf;
  return cached;
}

static void draw_glow_text(int x, int y, const char* s, uint32_t glow, uint32_t main, uint32_t bg) {
  (void)bg;
  /* Draw glow WITHOUT filling the background (prevents the "boxed" look). */
  const uint8_t OUTER_A = 46;
  const uint8_t INNER_A = 92;

  /* outer glow (2px) */
  gfx_draw_text_ta(x - 2, y, s, glow, OUTER_A);
  gfx_draw_text_ta(x + 2, y, s, glow, OUTER_A);
  gfx_draw_text_ta(x, y - 2, s, glow, OUTER_A);
  gfx_draw_text_ta(x, y + 2, s, glow, OUTER_A);
  gfx_draw_text_ta(x - 2, y - 1, s, glow, OUTER_A);
  gfx_draw_text_ta(x + 2, y - 1, s, glow, OUTER_A);
  gfx_draw_text_ta(x - 2, y + 1, s, glow, OUTER_A);
  gfx_draw_text_ta(x + 2, y + 1, s, glow, OUTER_A);

  /* inner glow (1px) */
  gfx_draw_text_ta(x - 1, y, s, glow, INNER_A);
  gfx_draw_text_ta(x + 1, y, s, glow, INNER_A);
  gfx_draw_text_ta(x, y - 1, s, glow, INNER_A);
  gfx_draw_text_ta(x, y + 1, s, glow, INNER_A);

  /* main */
  gfx_draw_text_t(x, y, s, main);
}

void splash_show(const boot_info_t* bi, unsigned duration_ticks) {
  (void)bi;

  /* user palette */
  const uint32_t PINK   = 0x00FF97B5u; /* #FF97B5 */
  const uint32_t PURPLE = 0x006E2A85u; /* #6E2A85 */

  /* UI tones */
  const uint32_t WHITE  = 0x00FFF4FAu;
  const uint32_t SUB    = 0x00E7C9D7u;
  const uint32_t BAR_BG = 0x00120A1Cu;
  const uint32_t BAR_ED = 0x00231133u;

  /* Draw logo at native resolution (crisp). */
  const int LW = LEIURUS_LOGO_W;
  const int LH = LEIURUS_LOGO_H;

  uint64_t start = timer_ticks();
  uint64_t end   = start + (uint64_t)duration_ticks;

  int sw = gfx_w();
  int sh = gfx_h();

  int lx = (sw - LW) / 2;
  int ly = (sh - LH) / 2 - 62;
  if (ly < 10) ly = 10;

  const char* title = "LOTUS";
  const char* sub   = "Rodina Amr Mukhtar";

  while (timer_ticks() < end) {
    event_t ev;
    if (events_pop(&ev)) {
      if (should_skip_on_event(&ev)) break;
    }

    /* background: gradient + subtle dark glass */
    gfx_fill_rect_vgrad(0, 0, sw, sh, PINK, PURPLE);
    gfx_fill_rect_a(0, 0, sw, sh, 0x00000000u, 135);

    /* soft blobs */
    gfx_fill_round_rect_a(-sw/6, -sh/5, sw/2, sh/2, sw/10, PINK, 22);
    gfx_fill_round_rect_a(sw - sw/3, -sh/7, sw/2, sh/2, sw/11, PURPLE, 22);
    gfx_fill_round_rect_a(sw/4, sh/2, sw/2, sh/2, sw/9, PINK, 14);

    /* logo with proper alpha blending (no pixelation / no black halo) */
    const uint32_t* argb = get_logo_argb();
    if (argb) gfx_blit_argb(lx, ly, LW, LH, argb, LW);

    /* Centering (8px per char) */
    int title_x = sw/2 - (8 * 5)/2;
    int sub_x   = sw/2 - (8 * 17)/2;

    int title_y = ly + LH + 10;
    int sub_y   = title_y + 22;

    draw_glow_text(title_x, title_y, title, PURPLE, WHITE, 0x00000000u);
    draw_glow_text(sub_x, sub_y, sub,   PINK,   SUB,   0x00000000u);

    /* progress bar */
    uint64_t now = timer_ticks();
    uint32_t total = duration_ticks ? duration_ticks : 1;
    uint32_t done  = (uint32_t)(now - start);
    if (done > total) done = total;

    int bar_w = 520;
    int bar_h = 12;
    int bx = sw/2 - bar_w/2;
    int by = sub_y + 38;

    /* bar glow behind */
    gfx_fill_rect_a(bx - 10, by - 6, bar_w + 20, bar_h + 12, PINK, 18);

    gfx_fill_rect(bx, by, bar_w, bar_h, BAR_BG);
    gfx_fill_rect(bx, by, bar_w, 1, BAR_ED);
    gfx_fill_rect(bx, by + bar_h - 1, bar_w, 1, BAR_ED);

    int fill = (int)((done * (uint32_t)bar_w) / total);
    if (fill < 0) fill = 0;
    if (fill > bar_w) fill = bar_w;

    /* fill: pink with a purple cap */
    gfx_fill_rect(bx, by, fill, bar_h, PINK);
    if (fill > 0) gfx_fill_rect(bx + fill - 4, by, 4, bar_h, PURPLE);
    if (fill > 0) gfx_fill_rect(bx, by + 2, fill, 2, 0x00FFFFFFu);

    gfx_present();
    __asm__ volatile("hlt");
  }
}
