#include "wm.h"
#include "gfx.h"
#include "terminal.h"
#include "ui.h"
#include "keys.h"
#include "timer.h"
#include "io.h"
#include "vfs.h"
#include "fat32.h"

#include <stdint.h>

typedef enum { WIN_TERM = 1, WIN_DEMO = 2, WIN_NOTE = 3, WIN_EXPL = 4, WIN_TRASH = 5 } win_type_t;
typedef enum {
  HT_NONE = 0, HT_CLIENT, HT_TITLE, HT_MIN, HT_MAX, HT_CLOSE,
  HT_L, HT_R, HT_T, HT_B, HT_TL, HT_TR, HT_BL, HT_BR
} hit_t;

typedef enum { WS_NORMAL=0, WS_MAX=1, WS_SNAP_L=2, WS_SNAP_R=3 } win_state_t;

typedef struct window {
  int x, y, w, h;
  const char* title;
  int visible;
  int opened;
  win_type_t type;

  win_state_t state;
  int sx, sy, sw, sh;

  int dragging;
  int drag_off_x, drag_off_y;

  int resizing;
  hit_t resize_hit;
  int rx0, ry0, rw0, rh0;
  int mdown_x, mdown_y;

  uint64_t last_title_click_ts; /* ms */
  int last_title_click_x, last_title_click_y;

  ui_textbox_t tb;
  ui_button_t  btn;

  int note_focused;
  int note_scroll;
  int note_caret;
  int note_len;
  char note[8192];

  int list_sel;
  int list_scroll;
  int item_count;
  char items[64][32];
  uint8_t item_isdir[64];
  uint32_t item_size[64];
  char path[64];

  /* explorer state */
  uint32_t dir_cluster;
  uint32_t fs_gen_seen;

  /* notepad file path */
  char note_path[64];
} window_t;

/* forward (implemented later) */
void __wm_explorer_refresh(window_t* w, int keep_sel);

#define WIN_MAX 5
static window_t wins[WIN_MAX];
static int zorder[WIN_MAX] = {0, 1, 2, 3, 4};
static int active = 0;

static int mouse_x = 10, mouse_y = 10;
static uint8_t last_buttons = 0;

static int start_open = 0;
static int start_hot = -1;

static int show_desktop = 0;
static uint32_t show_desktop_mask = 0;

static int desktop_menu_open = 0;
static int desktop_menu_x = 0;
static int desktop_menu_y = 0;
static int desktop_menu_hot = -1;

static uint32_t g_redraw_flags = WM_RF_FULL;
static int dirty_valid = 0;
static int dirty_x, dirty_y, dirty_w, dirty_h;

static inline int iabs(int v) { return v < 0 ? -v : v; }

static int cstr_len(const char* s) {
  int n = 0;
  while (s && s[n]) n++;
  return n;
}

static void rect_union(int ax, int ay, int aw, int ah,
                       int bx, int by, int bw, int bh,
                       int* ox, int* oy, int* ow, int* oh) {
  int x0 = (ax < bx) ? ax : bx;
  int y0 = (ay < by) ? ay : by;
  int x1 = ((ax + aw) > (bx + bw)) ? (ax + aw) : (bx + bw);
  int y1 = ((ay + ah) > (by + bh)) ? (ay + ah) : (by + bh);
  if (ox) *ox = x0;
  if (oy) *oy = y0;
  if (ow) *ow = x1 - x0;
  if (oh) *oh = y1 - y0;
}

static void set_dirty_union(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (!dirty_valid) {
    dirty_valid = 1;
    dirty_x = x; dirty_y = y; dirty_w = w; dirty_h = h;
  } else {
    int rx, ry, rw, rh;
    rect_union(dirty_x, dirty_y, dirty_w, dirty_h, x, y, w, h, &rx, &ry, &rw, &rh);
    dirty_x = rx; dirty_y = ry; dirty_w = rw; dirty_h = rh;
  }
}

int wm_take_dirty_rect(int* x, int* y, int* w, int* h) {
  if (!dirty_valid) return 0;
  if (x) *x = dirty_x;
  if (y) *y = dirty_y;
  if (w) *w = dirty_w;
  if (h) *h = dirty_h;
  dirty_valid = 0;
  return 1;
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int title_h(void) { return gfx_font_h() + 14; }
static int shadow_off(void) { return 18; }
static int resize_grab(void) { return 7; }

static int work_w(void) { return gfx_w(); }
static int work_h(void) { return gfx_h() - 84; }

/* accents stay neon in both themes */
static const uint32_t ACCENT_CYAN   = 0x00FFD4E8u; /* highlight (soft pink) */
static const uint32_t ACCENT_PURPLE = 0x006E2A85u; /* #6E2A85 */
static const uint32_t ACCENT_PINK   = 0x00FF97B5u; /* #FF97B5 */
static const uint32_t ACCENT_SOFT   = 0x001C0A26u; /* deep plum */

typedef struct theme_palette {
  uint32_t bg0, bg2, grid;
  uint32_t text, text_dim, text_muted;
  uint32_t win_bg, win_bg2, win_border, win_edge, win_edge_dim;
  uint32_t dock_bg, panel_bg;
  uint32_t shadow1, shadow2;
} theme_t;

static const theme_t THEME_DARK = {
  /* wallpaper */
  0x00110619u, 0x00090412u, 0x001A0E2Eu,
  /* text */
  0x00FFF4FAu, 0x00CDA0B6u, 0x00856A84u,
  /* window */
  0x000F0A18u, 0x00160D28u, 0x003B2452u, 0x00FF97B5u, 0x006E2A85u,
  /* dock/panels */
  0x000B0712u, 0x00110A1Du,
  /* shadows */
  0x0006030Bu, 0x00040208u
};

static const theme_t THEME_LIGHT = {
  /* wallpaper */
  0x00FFF1F7u, 0x00F2E3FFu, 0x00E4CFF2u,
  /* text */
  0x00110A1Du, 0x005A3B6Au, 0x00785C84u,
  /* window */
  0x00FFFFFFu, 0x00FFF7FBu, 0x00D8B9D6u, 0x006E2A85u, 0x00B06B92u,
  /* dock/panels */
  0x00FFE9F3u, 0x00FFF2F8u,
  /* shadows */
  0x00231233u, 0x001B1028u
};

static int g_theme_light = 0;
static inline const theme_t* TH(void) { return g_theme_light ? &THEME_LIGHT : &THEME_DARK; }

#define COL_BG0      (TH()->bg0)
#define COL_BG2      (TH()->bg2)
#define COL_GRID     (TH()->grid)
#define TEXT         (TH()->text)
#define TEXT_DIM     (TH()->text_dim)
#define TEXT_MUTED   (TH()->text_muted)
#define WIN_BG       (TH()->win_bg)
#define WIN_BG2      (TH()->win_bg2)
#define WIN_BORDER   (TH()->win_border)
#define WIN_EDGE     (TH()->win_edge)
#define WIN_EDGE_DIM (TH()->win_edge_dim)
#define DOCK_BG      (TH()->dock_bg)
#define PANEL_BG     (TH()->panel_bg)
#define SHADOW1      (TH()->shadow1)
#define SHADOW2      (TH()->shadow2)

static void toggle_theme(void) {
  g_theme_light ^= 1;
  term_set_theme(g_theme_light);
  g_redraw_flags |= WM_RF_FULL;
}

/* ---------- Z order ---------- */

static void bring_to_front(int idx) {
  if (zorder[WIN_MAX - 1] == idx) return;
  for (int i = 0; i < WIN_MAX; i++) {
    if (zorder[i] == idx) {
      for (int j = i; j < WIN_MAX - 1; j++) zorder[j] = zorder[j + 1];
      zorder[WIN_MAX - 1] = idx;
      return;
    }
  }
}

static int topmost_at(int x, int y) {
  for (int zi = WIN_MAX - 1; zi >= 0; zi--) {
    int i = zorder[zi];
    window_t* w = &wins[i];
    if (!w->visible) continue;
    if (in_rect(x, y, w->x, w->y, w->w, w->h)) return i;
  }
  return -1;
}

/* ---------- Geometry ---------- */

static void clamp_window(window_t* w) {
  int sw = work_w();
  int sh = work_h();
  int min_w = 280;
  int min_h = title_h() + 180;

  if (w->w < min_w) w->w = min_w;
  if (w->h < min_h) w->h = min_h;

  if (w->x < 0) w->x = 0;
  if (w->y < 0) w->y = 0;
  if (w->x + w->w > sw) w->x = sw - w->w;
  if (w->y + w->h > sh) w->y = sh - w->h;
  if (w->x < 0) w->x = 0;
  if (w->y < 0) w->y = 0;
}

void wm_get_paint_rect(int idx, int* x, int* y, int* w, int* h) {
  if (idx < 0 || idx >= WIN_MAX || !wins[idx].visible) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    return;
  }

  int pad = 24;
  int off = shadow_off();
  int rx = wins[idx].x - pad;
  int ry = wins[idx].y - pad;
  int rw = wins[idx].w + pad * 2 + off;
  int rh = wins[idx].h + pad * 2 + off;

  if (rx < 0) rx = 0;
  if (ry < 0) ry = 0;
  if (rx + rw > gfx_w()) rw = gfx_w() - rx;
  if (ry + rh > gfx_h()) rh = gfx_h() - ry;
  if (rw < 0) rw = 0;
  if (rh < 0) rh = 0;

  if (x) *x = rx;
  if (y) *y = ry;
  if (w) *w = rw;
  if (h) *h = rh;
}

static void save_rect_if_needed(window_t* w) {
  if (!w) return;
  if (w->state == WS_NORMAL) {
    w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
  }
}

static void restore_to_saved(window_t* w) {
  if (!w) return;
  w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
  w->state = WS_NORMAL;
  clamp_window(w);
}

static void maximize_window(window_t* w) {
  if (!w) return;
  save_rect_if_needed(w);
  w->x = 18; w->y = 18;
  w->w = work_w() - 36;
  w->h = work_h() - 36;
  w->state = WS_MAX;
}

static void snap_left(window_t* w) {
  if (!w) return;
  save_rect_if_needed(w);
  w->x = 12; w->y = 12;
  w->w = (work_w() / 2) - 18;
  w->h = work_h() - 24;
  w->state = WS_SNAP_L;
}

static void snap_right(window_t* w) {
  if (!w) return;
  save_rect_if_needed(w);
  int half = work_w() / 2;
  w->x = half + 6;
  w->y = 12;
  w->w = half - 18;
  w->h = work_h() - 24;
  w->state = WS_SNAP_R;
}

static void toggle_max_restore(window_t* w) {
  if (!w) return;
  if (w->state == WS_MAX) restore_to_saved(w);
  else maximize_window(w);
}

/* ---------- Hit test ---------- */

static void title_btn_rect(const window_t* w, int slot, int* bx, int* by, int* bs) {
  int s = gfx_font_h() + 2;
  int gap = 8;
  int x = w->x + w->w - 14 - (slot + 1) * s - slot * gap;
  int y = w->y + 9;
  if (bx) *bx = x;
  if (by) *by = y;
  if (bs) *bs = s;
}

static void min_btn_rect(const window_t* w, int* bx, int* by, int* bs) { title_btn_rect(w, 2, bx, by, bs); }
static void max_btn_rect(const window_t* w, int* bx, int* by, int* bs) { title_btn_rect(w, 1, bx, by, bs); }
static void close_btn_rect(const window_t* w, int* bx, int* by, int* bs) { title_btn_rect(w, 0, bx, by, bs); }

static hit_t hit_test(window_t* w, int mx, int my) {
  int th = title_h();
  int grab = resize_grab();

  int bx, by, bs;
  close_btn_rect(w, &bx, &by, &bs);
  if (in_rect(mx, my, bx, by, bs, bs)) return HT_CLOSE;
  max_btn_rect(w, &bx, &by, &bs);
  if (in_rect(mx, my, bx, by, bs, bs)) return HT_MAX;
  min_btn_rect(w, &bx, &by, &bs);
  if (in_rect(mx, my, bx, by, bs, bs)) return HT_MIN;

  int left   = in_rect(mx, my, w->x, w->y, grab, w->h);
  int right  = in_rect(mx, my, w->x + w->w - grab, w->y, grab, w->h);
  int top    = in_rect(mx, my, w->x, w->y, w->w, grab);
  int bottom = in_rect(mx, my, w->x, w->y + w->h - grab, w->w, grab);

  if (top && left) return HT_TL;
  if (top && right) return HT_TR;
  if (bottom && left) return HT_BL;
  if (bottom && right) return HT_BR;
  if (left) return HT_L;
  if (right) return HT_R;
  if (top) return HT_T;
  if (bottom) return HT_B;

  if (in_rect(mx, my, w->x + 1, w->y + 1, w->w - 2, th)) return HT_TITLE;
  return HT_CLIENT;
}

/* ---------- Drawing ---------- */

static void draw_wallpaper(void) {
  int sw = gfx_w();
  int sh = gfx_h();

  gfx_fill_rect_vgrad(0, 0, sw, sh, COL_BG0, COL_BG2);

  /* neon blobs (same in both themes, softer in light) */
  uint8_t a0 = g_theme_light ? 16 : 30;
  uint8_t a1 = g_theme_light ? 12 : 24;
  uint8_t a2 = g_theme_light ? 10 : 18;

  gfx_fill_round_rect_a(-sw / 9, -sh / 5, sw / 2, sh / 2, sw / 10, ACCENT_PURPLE, a0);
  gfx_fill_round_rect_a(sw - sw / 3, -sh / 7, sw / 2, sh / 2, sw / 11, ACCENT_CYAN, a1);
  gfx_fill_round_rect_a(sw / 3, sh / 2, sw / 2, sh / 2, sw / 9, ACCENT_PINK, a2);

  for (int y = 0; y < sh; y += 90) gfx_fill_rect_a(0, y, sw, 1, COL_GRID, g_theme_light ? 34 : 70);
  for (int x = 0; x < sw; x += 120) gfx_fill_rect_a(x, 0, 1, sh, COL_GRID, g_theme_light ? 22 : 36);

  gfx_fill_rect_a(0, 0, sw, 120, 0x00FFFFFFu, g_theme_light ? 18 : 8);
}

static void draw_shadow(const window_t* w) {
  for (int i = 0; i < 5; i++) {
    int spread = 8 + i * 3;
    int alpha = (g_theme_light ? 16 : 24) - i * 3;
    if (alpha < 6) alpha = 6;
    gfx_fill_round_rect_a(w->x - spread + 8, w->y - spread + 12,
                          w->w + spread * 2, w->h + spread * 2,
                          22 + i * 2, SHADOW2, (uint8_t)alpha);
  }
}

static void draw_window_frame(const window_t* w, int focused) {
  int th = title_h();
  int radius = 18;

  uint32_t edge = focused ? WIN_EDGE : WIN_EDGE_DIM;
  uint32_t edge2 = focused ? ACCENT_PURPLE : WIN_BORDER;

  uint32_t title_top = 0, title_bot = 0;
  if (!g_theme_light) {
    title_top = focused ? 0x00240A3Au : 0x001A082Bu;
    title_bot = focused ? 0x00120722u : 0x0010051Au;
  } else {
    title_top = focused ? 0x00E4EFFFu : 0x00EFF6FFu;
    title_bot = focused ? 0x00D7E6FFu : 0x00E3EEFFu;
  }

  draw_shadow(w);

  if (focused) {
    uint8_t pulse = (uint8_t)(12 + ((timer_ticks() / 6ULL) % 7ULL) * 3ULL);
    gfx_fill_round_rect_a(w->x - 3, w->y - 3, w->w + 6, w->h + 6, radius + 3, ACCENT_CYAN, pulse);
    gfx_fill_round_rect_a(w->x - 1, w->y - 1, w->w + 2, w->h + 2, radius + 2, ACCENT_PURPLE, pulse / 2);
  }

  gfx_fill_round_rect_vgrad(w->x, w->y, w->w, w->h, radius, WIN_BG2, WIN_BG);
  gfx_draw_round_rect(w->x, w->y, w->w, w->h, radius, edge);
  gfx_draw_round_rect(w->x + 1, w->y + 1, w->w - 2, w->h - 2, radius - 1, edge2);

  gfx_fill_round_rect_vgrad(w->x + 1, w->y + 1, w->w - 2, th + 6, radius - 1, title_top, title_bot);
  gfx_fill_rect_a(w->x + 10, w->y + th + 2, w->w - 20, 1, focused ? ACCENT_CYAN : WIN_BORDER, 180);
  gfx_fill_rect_a(w->x + 10, w->y + 6, w->w - 92, 1, 0x00FFFFFFu, g_theme_light ? 26 : 36);

  /* Title is drawn over a gradient header -> use transparent text to avoid "character boxes". */
  gfx_draw_text_ta(w->x + 17, w->y + 9, w->title, 0x00000000u, g_theme_light ? 90 : 120);
  gfx_draw_text_t(w->x + 16, w->y + 8, w->title, TEXT);

  int bx, by, bs;

  min_btn_rect(w, &bx, &by, &bs);
  gfx_fill_round_rect_a(bx, by, bs, bs, 7, PANEL_BG, g_theme_light ? 190 : 235);
  gfx_draw_round_rect(bx, by, bs, bs, 7, ACCENT_CYAN);
  gfx_fill_rect(bx + 5, by + bs - 7, bs - 10, 2, ACCENT_CYAN);

  max_btn_rect(w, &bx, &by, &bs);
  gfx_fill_round_rect_a(bx, by, bs, bs, 7, PANEL_BG, g_theme_light ? 190 : 235);
  gfx_draw_round_rect(bx, by, bs, bs, 7, ACCENT_PURPLE);
  gfx_draw_round_rect(bx + 5, by + 5, bs - 10, bs - 10, 4, ACCENT_PURPLE);

  close_btn_rect(w, &bx, &by, &bs);
  gfx_fill_round_rect_a(bx, by, bs, bs, 7, PANEL_BG, g_theme_light ? 190 : 240);
  gfx_draw_round_rect(bx, by, bs, bs, 7, ACCENT_PINK);
  for (int i = 0; i < bs - 12; i++) {
    gfx_fill_rect(bx + 6 + i, by + 6 + i, 1, 1, ACCENT_PINK);
    gfx_fill_rect(bx + bs - 7 - i, by + 6 + i, 1, 1, ACCENT_PINK);
  }
}

/* Preview client: used during drag/resize (super smooth) */
static void draw_preview_client(const window_t* w) {
  int th = title_h();
  int pad = 12;

  int cx = w->x + pad;
  int cy = w->y + th + pad;
  int cw = w->w - pad * 2;
  int ch = w->h - th - pad * 2;
  if (cw <= 0 || ch <= 0) return;

  gfx_fill_round_rect_a(cx, cy, cw, ch, 12, PANEL_BG, g_theme_light ? 210 : 170);
  gfx_draw_round_rect(cx, cy, cw, ch, 12, WIN_BORDER);
  gfx_fill_rect_a(cx + 12, cy + 12, cw - 24, 1, ACCENT_CYAN, 70);

  const char* t = w->dragging ? "Moving..." : "Resizing...";
  gfx_draw_text(cx + 18, cy + 18, t, TEXT_DIM, PANEL_BG);

  /* subtle stripes */
  for (int y = cy + 44; y < cy + ch - 12; y += 18) {
    gfx_fill_rect_a(cx + 14, y, cw - 28, 1, 0x00FFFFFFu, g_theme_light ? 10 : 8);
  }
}

/* ---------- Desktop icons ---------- */

typedef struct desktop_icon {
  int x, y;
  int win_idx;
  const char* label;
} desktop_icon_t;

static desktop_icon_t icons[] = {
  { 40,  40, WM_WIN_EXPL,  "My Computer" },
  { 40, 140, WM_WIN_NOTE,  "Notepad"     },
  { 40, 240, WM_WIN_TERM,  "Terminal"    },
  { 40, 340, WM_WIN_TRASH, "Trash"       },
  { 40, 440, WM_WIN_DEMO,  "Demo"        },
};
static const int ICON_COUNT = (int)(sizeof(icons)/sizeof(icons[0]));

static int icon_hot = -1;
static int icon_sel = 0;

/* double click timing in ms */
static uint64_t last_icon_click_ms = 0;
static int last_icon_click_idx = -1;
static int last_icon_click_x = 0;
static int last_icon_click_y = 0;

/* drag state */
static int icon_dragging = 0;
static int icon_drag_idx = -1;
static int icon_drag_start_mx = 0;
static int icon_drag_start_my = 0;
static int icon_drag_start_x  = 0;
static int icon_drag_start_y  = 0;
static int icon_drag_moved = 0;

/* desktop focus: Enter opens selected icon */
static int desktop_focus = 1;

static void icon_rect(int idx, int* x, int* y, int* w, int* h) {
  const int IW = 64;
  const int IH = 64;
  const int LH = gfx_font_h() + 10;
  int rx = icons[idx].x;
  int ry = icons[idx].y;
  int rw = IW;
  int rh = IH + LH;
  if (x) *x = rx;
  if (y) *y = ry;
  if (w) *w = rw;
  if (h) *h = rh;
}

static void icon_snap_to_grid(int* x, int* y) {
  const int gx = 90;
  const int gy = 100;

  int nx = (*x + gx/2) / gx;
  int ny = (*y + gy/2) / gy;

  *x = nx * gx;
  *y = ny * gy;

  if (*x < 28) *x = 28;
  if (*y < 24) *y = 24;
  if (*x > gfx_w() - 140) *x = gfx_w() - 140;
  if (*y > work_h() - 160) *y = work_h() - 160;
}

static int hit_icon(int mx, int my) {
  for (int i = 0; i < ICON_COUNT; i++) {
    int rx, ry, rw, rh;
    icon_rect(i, &rx, &ry, &rw, &rh);
    if (in_rect(mx, my, rx, ry, rw, rh)) return i;
  }
  return -1;
}

static void draw_icon_tile(int x, int y, int sel, int hot) {
  uint32_t top = sel ? (g_theme_light ? 0x00E4EFFFu : 0x00152243u)
                     : (g_theme_light ? 0x00EFF6FFu : 0x00111A31u);
  uint32_t bot = sel ? (g_theme_light ? 0x00D7E6FFu : 0x000E1730u)
                     : (g_theme_light ? 0x00E3EEFFu : 0x000C1328u);

  uint32_t edge = sel ? ACCENT_CYAN : (hot ? ACCENT_PURPLE : WIN_BORDER);

  gfx_fill_round_rect_a(x + 5, y + 8, 68, 68, 18, SHADOW1, g_theme_light ? 90 : 120);
  gfx_fill_round_rect_vgrad(x, y, 64, 64, 16, top, bot);
  gfx_draw_round_rect(x, y, 64, 64, 16, edge);
  gfx_fill_rect_a(x + 8, y + 8, 48, 1, 0x00FFFFFFu, g_theme_light ? 22 : 36);
}

static void draw_icon_glyph(int kind, int x, int y) {
  if (kind == WM_WIN_EXPL) {
    gfx_draw_round_rect(x + 16, y + 20, 32, 22, 6, ACCENT_CYAN);
    gfx_fill_rect(x + 20, y + 28, 24, 2, ACCENT_CYAN);
    gfx_fill_rect(x + 24, y + 44, 16, 3, ACCENT_PURPLE);
  } else if (kind == WM_WIN_NOTE) {
    gfx_draw_round_rect(x + 18, y + 14, 28, 36, 6, ACCENT_PURPLE);
    for (int i = 0; i < 5; i++) gfx_fill_rect(x + 22, y + 22 + i * 5, 18, 1, ACCENT_CYAN);
  } else if (kind == WM_WIN_TERM) {
    gfx_draw_round_rect(x + 14, y + 18, 36, 24, 6, ACCENT_CYAN);
    /* transparent glyph over gradient */
    gfx_draw_text_ta(x + 21, y + 25, ">_", 0x00000000u, 110);
    gfx_draw_text_t(x + 20, y + 24, ">_", TEXT);
  } else if (kind == WM_WIN_TRASH) {
    gfx_draw_round_rect(x + 22, y + 20, 20, 28, 6, ACCENT_PINK);
    gfx_fill_rect(x + 20, y + 18, 24, 3, ACCENT_PURPLE);
    for (int i = 0; i < 3; i++) gfx_fill_rect(x + 26 + i * 4, y + 25, 1, 18, ACCENT_CYAN);
  } else {
    gfx_fill_rect(x + 30, y + 14, 4, 34, ACCENT_CYAN);
    gfx_fill_rect(x + 16, y + 29, 32, 4, ACCENT_PURPLE);
  }
}

static void draw_desktop_icons(void) {
  for (int i = 0; i < ICON_COUNT; i++) {
    int x = icons[i].x;
    int y = icons[i].y;

    int is_hot = (i == icon_hot);
    int is_sel = (i == icon_sel);

    draw_icon_tile(x, y, is_sel, is_hot);
    draw_icon_glyph(icons[i].win_idx, x, y);

    /* label: no framed box. Selected icon gets a subtle glow pill only (no border). */
    const char* lbl = icons[i].label;
    int lw = cstr_len(lbl) * 8;
    int fh = gfx_font_h();
    int tx = x + (64 - lw) / 2;
    if (tx < 6) tx = 6;
    if (tx + lw > gfx_w() - 6) tx = gfx_w() - 6 - lw;
    int ty = y + 78;

    if (is_sel) {
      int pill_w = lw + 18;
      if (pill_w < 54) pill_w = 54;
      if (pill_w > 110) pill_w = 110;
      int px = x + (64 - pill_w) / 2;
      int py = y + 74;
      gfx_fill_round_rect_a(px, py, pill_w, fh + 10, 12, ACCENT_PURPLE, g_theme_light ? 28 : 22);
      gfx_fill_round_rect_a(px, py, pill_w, fh + 10, 12, 0x00FFFFFFu, g_theme_light ? 18 : 10);
    }

    gfx_draw_text_ta(tx + 1, ty + 1, lbl, 0x00000000u, g_theme_light ? 90 : 140);
    gfx_draw_text_t(tx, ty, lbl, is_sel ? TEXT : TEXT_DIM);
  }
}

static void open_app_window(int win_idx) {
  if (win_idx < 0 || win_idx >= WIN_MAX) return;
  wins[win_idx].opened = 1;
  wins[win_idx].visible = 1;
  active = win_idx;
  bring_to_front(win_idx);
  show_desktop = 0;
  desktop_focus = 0;
  if (win_idx == WM_WIN_EXPL) {
    __wm_explorer_refresh(&wins[WM_WIN_EXPL], 1);
  }
  g_redraw_flags |= WM_RF_FULL;
}

/* ---------- Notepad ---------- */

static void notepad_client_rect(const window_t* w, int* x, int* y, int* cw, int* ch) {
  int th = title_h();
  int pad = 12;
  int cx = w->x + pad;
  int cy = w->y + th + pad;
  int ww = w->w - pad * 2;
  int hh = w->h - th - pad * 2;
  if (x) *x = cx;
  if (y) *y = cy;
  if (cw) *cw = ww;
  if (ch) *ch = hh;
}

static int notepad_get_line_of_index(const window_t* w, int idx) {
  int line = 0;
  for (int i = 0; i < idx && i < w->note_len; i++) if (w->note[i] == '\n') line++;
  return line;
}

static void notepad_ensure_visible(window_t* w, int visible_rows) {
  int caret_line = notepad_get_line_of_index(w, w->note_caret);
  if (caret_line < w->note_scroll) w->note_scroll = caret_line;
  if (caret_line >= w->note_scroll + visible_rows) w->note_scroll = caret_line - (visible_rows - 1);
  if (w->note_scroll < 0) w->note_scroll = 0;
}

static void notepad_insert(window_t* w, char c) {
  if (w->note_len >= (int)sizeof(w->note) - 1) return;
  if (c == '\r') c = '\n';
  if (c == '\t') c = ' ';
  if (c == '\n' || (c >= 32 && c <= 126)) {
    for (int i = w->note_len; i > w->note_caret; i--) w->note[i] = w->note[i - 1];
    w->note[w->note_caret] = c;
    w->note_len++;
    w->note_caret++;
    w->note[w->note_len] = 0;
  }
}

static void notepad_backspace(window_t* w) {
  if (w->note_caret <= 0) return;
  for (int i = w->note_caret - 1; i < w->note_len; i++) w->note[i] = w->note[i + 1];
  w->note_caret--;
  w->note_len--;
  if (w->note_len < 0) w->note_len = 0;
  if (w->note_caret > w->note_len) w->note_caret = w->note_len;
}

static void notepad_delete(window_t* w) {
  if (w->note_caret >= w->note_len) return;
  for (int i = w->note_caret; i < w->note_len; i++) w->note[i] = w->note[i + 1];
  w->note_len--;
  if (w->note_len < 0) w->note_len = 0;
  if (w->note_caret > w->note_len) w->note_caret = w->note_len;
}

static void notepad_new_file(window_t* w) {
  if (!w) return;
  w->note_len = 0;
  w->note_caret = 0;
  w->note_scroll = 0;
  w->note[0] = 0;
  /* default path */
  const char* defp = "C:\\NOTE.TXT";
  int i = 0;
  while (defp[i] && i < (int)sizeof(w->note_path) - 1) { w->note_path[i] = defp[i]; i++; }
  w->note_path[i] = 0;
}

static void notepad_save_file(window_t* w) {
  if (!w) return;
  if (!w->note_path[0]) {
    notepad_new_file(w);
  }

  char drv = 'C';
  const char* sub = vfs_strip_drive(w->note_path, 'C', &drv);
  uint32_t base = vfs_root_cluster(drv);

  /* write note buffer (LF only) */
  (void)fat32_write(base, sub, (const uint8_t*)w->note, (uint32_t)w->note_len, 0);
}

static void notepad_move_left(window_t* w) { if (w->note_caret > 0) w->note_caret--; }
static void notepad_move_right(window_t* w) { if (w->note_caret < w->note_len) w->note_caret++; }

static int notepad_index_from_line_col(const window_t* w, int target_line, int target_col) {
  int line = 0;
  int col = 0;
  for (int i = 0; i < w->note_len; i++) {
    char c = w->note[i];
    if (line == target_line) {
      if (c == '\n') return i;
      if (col == target_col) return i;
      col++;
    }
    if (c == '\n') { line++; col = 0; }
    if (line > target_line) return i;
  }
  return w->note_len;
}

static void draw_notepad_content(window_t* w) {
  int cx, cy, cw, ch;
  notepad_client_rect(w, &cx, &cy, &cw, &ch);

  gfx_fill_round_rect_vgrad(cx, cy, cw, ch, 12, DOCK_BG, PANEL_BG);
  gfx_draw_round_rect(cx, cy, cw, ch, 12, WIN_BORDER);
  gfx_fill_rect_a(cx + 10, cy + 10, cw - 20, 1, ACCENT_CYAN, 60);

  int fh = gfx_font_h();
  int cols = cw / 8;
  int rows = ch / fh;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;

  notepad_ensure_visible(w, rows);

  gfx_set_clip(cx + 4, cy + 4, cw - 8, ch - 8);

  int line = 0, col = 0;
  int caret_line = 0, caret_col = 0;
  {
    int l = 0, c = 0;
    for (int i = 0; i < w->note_caret && i < w->note_len; i++) {
      if (w->note[i] == '\n') { l++; c = 0; }
      else c++;
    }
    caret_line = l;
    caret_col = c;
  }

  for (int i = 0; i < w->note_len; i++) {
    char chh = w->note[i];

    if (line >= w->note_scroll) {
      int draw_line = line - w->note_scroll;
      if (draw_line >= rows) break;

      if (chh == '\n') { line++; col = 0; continue; }

      if (col < cols) {
        int px = cx + 8 + col * 8;
        int py = cy + 6 + draw_line * fh;
        gfx_draw_char(px, py, (uint8_t)chh, TEXT, PANEL_BG);
      }
      col++;
    } else {
      if (chh == '\n') { line++; col = 0; }
    }
  }

  if (w->note_focused) {
    int c_line = caret_line - w->note_scroll;
    if (c_line >= 0 && c_line < rows) {
      int px = cx + 8 + caret_col * 8;
      int py = cy + 6 + c_line * fh;
      gfx_fill_rect(px, py, 2, fh, ACCENT_CYAN);
      gfx_fill_rect(px + 2, py, 1, fh, ACCENT_PURPLE);
    }
  }

  gfx_reset_clip();
}

/* ---------- Explorer/Trash list ---------- */

static void list_client_rect(const window_t* w, int* x, int* y, int* cw, int* ch) {
  int th = title_h();
  int pad = 12;
  int cx = w->x + pad;
  int cy = w->y + th + pad + gfx_font_h() + 6;
  int ww = w->w - pad * 2;
  int hh = w->h - th - pad * 2 - gfx_font_h() - 6;
  if (x) *x = cx;
  if (y) *y = cy;
  if (cw) *cw = ww;
  if (ch) *ch = hh;
}

/* Real FAT32-backed explorer listing. */
static void explorer_refresh(window_t* w, int keep_sel) {
  if (!w) return;

  int old_sel = w->list_sel;
  fat32_listent_t ent[64];
  int n = 0;
  int rc = fat32_list_dir_cluster(w->dir_cluster, ent, 64, &n);
  if (rc != 0) {
    w->item_count = 1;
    w->list_sel = 0;
    w->list_scroll = 0;
    const char* e = "(error)";
    int i = 0; while (e[i] && i < 31) { w->items[0][i] = e[i]; i++; }
    w->items[0][i] = 0;
    w->item_isdir[0] = 0;
    w->item_size[0] = 0;
    w->fs_gen_seen = fat32_generation();
    return;
  }

  int out = 0;
  uint32_t root = vfs_root_cluster('C');
  if (w->dir_cluster != root && out < 64) {
    w->items[out][0] = '.'; w->items[out][1] = '.'; w->items[out][2] = 0;
    w->item_isdir[out] = 1;
    w->item_size[out] = 0;
    out++;
  }

  /* dirs first, then files */
  for (int pass = 0; pass < 2 && out < 64; pass++) {
    for (int i = 0; i < n && out < 64; i++) {
      if (!ent[i].name[0]) continue;
      /* skip dot entries */
      if (ent[i].name[0] == '.' && (ent[i].name[1] == 0 || (ent[i].name[1] == '.' && ent[i].name[2] == 0))) continue;

      int isdir = ent[i].is_dir ? 1 : 0;
      if ((pass == 0 && !isdir) || (pass == 1 && isdir)) continue;

      int j = 0;
      while (ent[i].name[j] && j < 31) { w->items[out][j] = ent[i].name[j]; j++; }
      w->items[out][j] = 0;
      w->item_isdir[out] = (uint8_t)isdir;
      w->item_size[out] = ent[i].size;
      out++;
    }
  }

  if (out <= 0) {
    w->item_count = 1;
    w->list_sel = 0;
    w->list_scroll = 0;
    const char* e = "(empty)";
    int i = 0; while (e[i] && i < 31) { w->items[0][i] = e[i]; i++; }
    w->items[0][i] = 0;
    w->item_isdir[0] = 0;
    w->item_size[0] = 0;
  } else {
    w->item_count = out;
    if (!keep_sel) w->list_sel = 0;
    else {
      w->list_sel = old_sel;
      if (w->list_sel < 0) w->list_sel = 0;
      if (w->list_sel >= w->item_count) w->list_sel = w->item_count - 1;
    }
    if (w->list_scroll > w->list_sel) w->list_scroll = w->list_sel;
    if (w->list_scroll < 0) w->list_scroll = 0;
  }

  w->fs_gen_seen = fat32_generation();
}

/* helper symbol used by open_app_window/explorer_open_selected (avoids C++ forward decl headaches) */
void __wm_explorer_refresh(window_t* w, int keep_sel) { explorer_refresh(w, keep_sel); }

static void explorer_build_root(window_t* w) {
  /* Start at C:\\ root and build a real FAT32 listing. */
  w->list_sel = 0;
  w->list_scroll = 0;
  w->item_count = 0;
  w->path[0] = 'C'; w->path[1] = ':'; w->path[2] = '\\'; w->path[3] = 0;
  w->dir_cluster = vfs_root_cluster('C');
  w->fs_gen_seen = fat32_generation();

  /* Populate list from disk. */
  /* forward */
  extern void __wm_explorer_refresh(window_t* w, int keep_sel);
  __wm_explorer_refresh(w, 0);
}

static void trash_build(window_t* w) {
  w->item_count = 1;
  w->list_sel = 0;
  w->list_scroll = 0;
  const char* a = "(empty)";
  int i = 0;
  for (; a[i] && i < 31; i++) w->items[0][i] = a[i];
  w->items[0][i] = 0;
  w->item_isdir[0] = 0;
  w->item_size[0] = 0;
  w->path[0] = 'T'; w->path[1] = 'r'; w->path[2] = 'a'; w->path[3] = 's'; w->path[4] = 'h'; w->path[5] = 0;
}

static void explorer_open_selected(window_t* w) {
  if (!w || w->item_count <= 0) return;
  if (w->list_sel < 0 || w->list_sel >= w->item_count) return;

  const char* name = w->items[w->list_sel];
  uint8_t is_dir = w->item_isdir[w->list_sel];

  /* parent entry */
  if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
    /* go up one directory */
    int len = cstr_len(w->path);
    if (len > 3) {
      /* trim trailing slash */
      if (w->path[len - 1] == '\\') { w->path[len - 1] = 0; len--; }
      while (len > 3 && w->path[len - 1] != '\\') { w->path[len - 1] = 0; len--; }
      if (len <= 3) { w->path[0]='C'; w->path[1]=':'; w->path[2]='\\'; w->path[3]=0; }
      /* ensure trailing slash */
      len = cstr_len(w->path);
      if (w->path[len - 1] != '\\' && len < (int)sizeof(w->path) - 1) {
        w->path[len] = '\\'; w->path[len + 1] = 0;
      }
    }

    /* resolve cluster from path */
    char drv = 'C';
    const char* sub = vfs_strip_drive(w->path, 'C', &drv);
    uint32_t root = vfs_root_cluster(drv);
    uint32_t cl = root;
    if (sub && sub[0] && !(sub[0] == '\\' && !sub[1])) {
      char tmp[64];
      int j = 0;
      while (sub[j] && j < (int)sizeof(tmp) - 1) { tmp[j] = sub[j]; j++; }
      tmp[j] = 0;
      /* drop trailing slash for resolver */
      if (j > 1 && (tmp[j - 1] == '\\' || tmp[j - 1] == '/')) tmp[j - 1] = 0;
      if (fat32_resolve_dir(root, tmp, &cl) != 0) cl = root;
    }
    w->dir_cluster = cl;
    extern void __wm_explorer_refresh(window_t* w, int keep_sel);
    __wm_explorer_refresh(w, 0);
    g_redraw_flags |= WM_RF_EXPL;
    return;
  }

  if (is_dir) {
    /* enter directory */
    uint32_t newclus = 0;
    if (fat32_resolve_dir(w->dir_cluster, name, &newclus) != 0) {
      /* show error row */
      w->item_count = 1;
      w->list_sel = 0;
      w->list_scroll = 0;
      w->items[0][0] = '('; w->items[0][1] = 'e'; w->items[0][2] = 'r'; w->items[0][3] = 'r';
      w->items[0][4] = 'o'; w->items[0][5] = 'r'; w->items[0][6] = ')'; w->items[0][7] = 0;
      w->item_isdir[0] = 0;
      w->item_size[0] = 0;
      g_redraw_flags |= WM_RF_EXPL;
      return;
    }

    /* append to path */
    int len = cstr_len(w->path);
    if (len < (int)sizeof(w->path) - 2) {
      if (w->path[len - 1] != '\\') { w->path[len++]='\\'; }
      for (int i=0; name[i] && len < (int)sizeof(w->path) - 2; i++) w->path[len++] = name[i];
      w->path[len++] = '\\';
      w->path[len] = 0;
    }
    w->dir_cluster = newclus;
    extern void __wm_explorer_refresh(window_t* w, int keep_sel);
    __wm_explorer_refresh(w, 0);
    g_redraw_flags |= WM_RF_EXPL;
    return;
  }

  /* open file into Notepad */
  window_t* n = &wins[WM_WIN_NOTE];
  n->note_len = 0;
  n->note_caret = 0;
  n->note_scroll = 0;
  n->note_focused = 1;

  /* build full path for notepad */
  int pl = 0;
  while (w->path[pl] && pl < (int)sizeof(n->note_path) - 2) { n->note_path[pl] = w->path[pl]; pl++; }
  if (pl > 0 && n->note_path[pl - 1] == '\\') { /* ok */ }
  else if (pl < (int)sizeof(n->note_path) - 1) { n->note_path[pl++]='\\'; }
  for (int i=0; name[i] && pl < (int)sizeof(n->note_path) - 1; i++) n->note_path[pl++] = name[i];
  n->note_path[pl] = 0;

  char drv = 'C';
  const char* sub = vfs_strip_drive(n->note_path, 'C', &drv);
  uint32_t base = vfs_root_cluster(drv);

  uint8_t* buf = 0;
  uint32_t sz = 0;
  int ok = (fat32_read_file(base, sub, &buf, &sz) == 0);

  if (!ok) {
    const char* msg = "(error) could not read file";
    int i = 0;
    while (msg[i] && i < (int)sizeof(n->note) - 1) { n->note[i] = msg[i]; i++; }
    n->note[i] = 0;
    n->note_len = i;
    n->note_caret = i;
  } else {
    uint32_t copy = sz;
    if (copy > (uint32_t)sizeof(n->note) - 1) copy = (uint32_t)sizeof(n->note) - 1;
    int j = 0;
    for (uint32_t i = 0; i < copy && j < (int)sizeof(n->note) - 1; i++) {
      char ch = (char)buf[i];
      if (ch == '\r') continue;
      n->note[j++] = ch;
    }
    n->note_len = j;
    n->note[n->note_len] = 0;
    n->note_caret = n->note_len;
  }

  wins[WM_WIN_NOTE].visible = 1;
  active = WM_WIN_NOTE;
  bring_to_front(WM_WIN_NOTE);
  desktop_focus = 0;
  g_redraw_flags |= WM_RF_NOTE | WM_RF_FULL;
}

static void draw_list_window_content(window_t* w) {
  int cx, cy, cw, ch;
  list_client_rect(w, &cx, &cy, &cw, &ch);

  gfx_fill_round_rect_vgrad(cx, cy, cw, ch, 12, DOCK_BG, PANEL_BG);
  gfx_draw_round_rect(cx, cy, cw, ch, 12, WIN_BORDER);

  gfx_fill_round_rect_a(cx + 10, cy + 8, cw - 20, gfx_font_h() + 10, 8, PANEL_BG, g_theme_light ? 235 : 240);
  gfx_draw_text(cx + 18, cy + 13, w->path, TEXT_MUTED, PANEL_BG);

  int fh = gfx_font_h();
  int rows = (ch - (gfx_font_h() + 24)) / (fh + 8);
  if (rows < 1) rows = 1;

  for (int i = 0; i < rows; i++) {
    int idx = w->list_scroll + i;
    if (idx >= w->item_count) break;

    int iy = cy + gfx_font_h() + 24 + i * (fh + 8);
    int sel = (idx == w->list_sel);
    uint32_t bg = sel ? (g_theme_light ? 0x00DCE9FFu : 0x00132046u)
                      : (g_theme_light ? 0x00EEF6FFu : 0x000F172Cu);
    uint32_t bd = sel ? ACCENT_CYAN : WIN_BORDER;

    gfx_fill_round_rect_a(cx + 10, iy, cw - 20, fh + 8, 10, bg, g_theme_light ? 250 : 235);
    gfx_draw_round_rect(cx + 10, iy, cw - 20, fh + 8, 10, bd);

    /* name */
    gfx_draw_text_t(cx + 22, iy + 4, w->items[idx], TEXT);

    /* size / <DIR> on the right */
    char sbuf[16];
    if (w->item_isdir[idx]) {
      sbuf[0] = '<'; sbuf[1] = 'D'; sbuf[2] = 'I'; sbuf[3] = 'R'; sbuf[4] = '>'; sbuf[5] = 0;
    } else {
      uint32_t v = w->item_size[idx];
      int p = 0;
      if (v == 0) sbuf[p++] = '0';
      else {
        char tmp[12]; int t = 0;
        while (v && t < 11) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t--) sbuf[p++] = tmp[t];
      }
      if (p < 14) { sbuf[p++] = 'b'; }
      sbuf[p] = 0;
    }
    int sl = cstr_len(sbuf);
    int sx = cx + cw - 22 - sl * 8;
    gfx_draw_text_t(sx, iy + 4, sbuf, TEXT_DIM);
  }
}

/* ---------- Demo ---------- */

static void demo_button_cb(void* userdata) {
  (void)userdata;
  term_write("\n[UI] Button clicked. Textbox=\"");
  term_write(wins[WM_WIN_DEMO].tb.text);
  term_write("\"\n");
  g_redraw_flags |= WM_RF_TERM;
}

static void draw_demo_content(window_t* w) {
  int th = title_h();
  int cx = w->x + 16;
  int cy = w->y + th + 16;

  gfx_fill_round_rect_a(cx, cy, w->w - 32, gfx_font_h() + 18, 12, PANEL_BG, g_theme_light ? 235 : 220);
  gfx_draw_round_rect(cx, cy, w->w - 32, gfx_font_h() + 18, 12, ACCENT_PURPLE);
  gfx_draw_text(cx + 12, cy + 9, "Neon control surface", TEXT, PANEL_BG);
  cy += gfx_font_h() + 28;

  w->tb.x = cx;
  w->tb.y = cy;
  w->tb.w = w->w - 32;
  w->tb.h = gfx_font_h() + 18;
  ui_textbox_draw(&w->tb);
  cy += w->tb.h + 16;

  w->btn.x = cx;
  w->btn.y = cy;
  w->btn.w = 244;
  w->btn.h = gfx_font_h() + 18;
  w->btn.label = "Send to terminal";
  w->btn.cb = demo_button_cb;
  w->btn.userdata = 0;
  ui_button_draw(&w->btn);
}

static void draw_terminal_content(const window_t* w) {
  int th = title_h();
  int pad = 12;

  int client_x = w->x + pad;
  int client_y = w->y + th + pad;
  int client_w = w->w - pad * 2;
  int client_h = w->h - th - pad * 2;

  term_render(client_x, client_y, client_w, client_h);
}

/* ---------- Dock + menus ---------- */

static void fmt_time(char out[9]) {
  uint64_t sec64 = timer_uptime_ms() / 1000ULL;
  uint32_t sec = (uint32_t)(sec64 % 60ULL);
  uint32_t min = (uint32_t)((sec64 / 60ULL) % 60ULL);
  uint32_t hr  = (uint32_t)((sec64 / 3600ULL) % 24ULL);

  out[0] = '0' + (hr / 10);
  out[1] = '0' + (hr % 10);
  out[2] = ':';
  out[3] = '0' + (min / 10);
  out[4] = '0' + (min % 10);
  out[5] = ':';
  out[6] = '0' + (sec / 10);
  out[7] = '0' + (sec % 10);
  out[8] = 0;
}

static void dock_rect(int* x, int* y, int* w, int* h) {
  int sh = gfx_h();
  int dh = 56;

  /* longer, near-full-width dock */
  int dw = gfx_w() - 28;
  if (dw < 540) dw = 540;
  if (dw > gfx_w() - 16) dw = gfx_w() - 16;

  int dx = (gfx_w() - dw) / 2;
  int dy = sh - dh - 14;
  if (x) *x = dx;
  if (y) *y = dy;
  if (w) *w = dw;
  if (h) *h = dh;
}

static void dock_right_boxes(int* x_clock, int* x_theme, int* x_show, int* x_power, int* y, int* h) {
  int dx, dy, dw, dh;
  dock_rect(&dx, &dy, &dw, &dh);

  int yy = dy + 10;
  int hh = 36;

  /* right-to-left: power, show desktop, theme, clock */
  int xp = dx + dw - 64;     /* power */
  int xs = xp - 60;          /* show desktop */
  int xt = xs - 60;          /* theme */
  int xc = xt - 96;          /* clock */

  if (x_clock) *x_clock = xc;
  if (x_theme) *x_theme = xt;
  if (x_show)  *x_show  = xs;
  if (x_power) *x_power = xp;
  if (y) *y = yy;
  if (h) *h = hh;
}

static inline int win_is_opened(int idx) {
  if (idx < 0 || idx >= WIN_MAX) return 0;
  return wins[idx].opened != 0;
}

static int dock_open_list(int out[WIN_MAX]) {
  int n = 0;
  for (int i = 0; i < WIN_MAX; i++) {
    if (win_is_opened(i)) out[n++] = i;
  }
  return n;
}

static int start_button_hit(int mx, int my) {
  int dx, dy, dw, dh;
  dock_rect(&dx, &dy, &dw, &dh);
  return in_rect(mx, my, dx + 12, dy + 10, 60, 36);
}

/* rect for the Nth open app button (slot index, not window index) */
static void app_slot_rect(int slot, int count, int* x, int* y, int* w, int* h) {
  int dx, dy, dw, dh;
  dock_rect(&dx, &dy, &dw, &dh);

  int xc, xt, xs, xp, yy, hh;
  dock_right_boxes(&xc, &xt, &xs, &xp, &yy, &hh);

  int by = dy + 10;
  int bh = 36;

  int apps_x0 = dx + 86;
  int apps_x1 = xc - 14;
  int avail = apps_x1 - apps_x0;

  int bw = 82;
  int gap = 10;

  int need = count * bw + (count > 0 ? (count - 1) * gap : 0);
  if (need > avail && count > 0) {
    /* shrink a bit if many windows are open */
    bw = (avail - (count - 1) * gap) / count;
    if (bw < 64) bw = 64;
  }

  int bx = apps_x0 + slot * (bw + gap);

  if (x) *x = bx;
  if (y) *y = by;
  if (w) *w = bw;
  if (h) *h = bh;
}

static int hit_app_button(int mx, int my) {
  int list[WIN_MAX];
  int n = dock_open_list(list);
  for (int s = 0; s < n; s++) {
    int bx, by, bw, bh;
    app_slot_rect(s, n, &bx, &by, &bw, &bh);
    if (in_rect(mx, my, bx, by, bw, bh)) return list[s];
  }
  return -1;
}

static int power_hit(int mx, int my) {
  int xc, xt, xs, xp, yy, hh;
  dock_right_boxes(&xc, &xt, &xs, &xp, &yy, &hh);
  return in_rect(mx, my, xp, yy, 52, hh);
}

static int start_menu_hit_item(int mx, int my) {
  if (!start_open) return -1;
  int dx, dy, dw, dh;
  dock_rect(&dx, &dy, &dw, &dh);

  int mw = 320;
  int mh = 244;
  int mx0 = dx;
  int my0 = dy - mh - 12;

  for (int i = 0; i < 6; i++) {
    int iy = my0 + 14 + i * 36;
    if (in_rect(mx, my, mx0 + 12, iy, mw - 24, 28)) return i;
  }
  return -1;
}

static int desktop_menu_hit_item(int mx, int my) {
  if (!desktop_menu_open) return -1;
  int mw = 260;
  int mh = 5 * 30 + 12;
  int mx0 = desktop_menu_x;
  int my0 = desktop_menu_y;
  if (mx0 + mw > gfx_w()) mx0 = gfx_w() - mw;
  if (my0 + mh > work_h()) my0 = work_h() - mh;

  for (int i = 0; i < 5; i++) {
    int iy = my0 + 6 + i * 30;
    if (in_rect(mx, my, mx0 + 6, iy, mw - 12, 24)) return i;
  }
  return -1;
}

static int pick_next_visible(int except) {
  for (int i = 0; i < WIN_MAX; i++) {
    if (i == except) continue;
    if (wins[i].visible) return i;
  }
  return WM_WIN_TERM;
}

static void show_desktop_toggle(void) {
  if (!show_desktop) {
    show_desktop_mask = 0;
    for (int i = 0; i < WIN_MAX; i++) {
      if (wins[i].visible) show_desktop_mask |= (1u << i);
      wins[i].visible = 0;
    }
    show_desktop = 1;
  } else {
    for (int i = 0; i < WIN_MAX; i++) {
      wins[i].visible = (show_desktop_mask & (1u << i)) ? 1 : 0;
    }
    show_desktop = 0;
  }
  desktop_focus = 1;
  g_redraw_flags |= WM_RF_FULL;
}


static void system_shutdown(void) {
  /* Best-effort shutdown/exit:
     1) QEMU debug-exit (requires -device isa-debug-exit,iobase=0xf4,iosize=0x04)
     2) ACPI ports (works on many QEMU setups)
     3) Halt */
  outl(0xF4, 0x10);

  outw(0x604, 0x2000);
  outw(0xB004, 0x2000);

  for (;;) { __asm__ volatile("cli; hlt"); }
}


static int show_desktop_hit(int mx, int my) {
  int xs, xt, xc, xp, yy, hh;
  dock_right_boxes(&xc, &xt, &xs, &xp, &yy, &hh);
  return in_rect(mx, my, xs, yy, 52, hh);
}

static int theme_hit(int mx, int my) {
  int xs, xt, xc, xp, yy, hh;
  dock_right_boxes(&xc, &xt, &xs, &xp, &yy, &hh);
  return in_rect(mx, my, xt, yy, 52, hh);
}

static void draw_dock(void) {
  int dx, dy, dw, dh;
  dock_rect(&dx, &dy, &dw, &dh);

  gfx_fill_round_rect_a(dx + 2, dy + 6, dw, dh, 24, SHADOW1, g_theme_light ? 90 : 150);
  gfx_fill_round_rect_vgrad(dx, dy, dw, dh, 24, DOCK_BG, PANEL_BG);
  gfx_draw_round_rect(dx, dy, dw, dh, 24, WIN_BORDER);
  gfx_fill_rect_a(dx + 14, dy + 8, dw - 28, 1, 0x00FFFFFFu, g_theme_light ? 18 : 26);

  /* Start button */
  uint32_t start_bg = start_open ? (g_theme_light ? 0x00DCE9FFu : 0x00152345u)
                                 : (g_theme_light ? 0x00EEF6FFu : 0x00111B34u);
  gfx_fill_round_rect_a(dx + 12, dy + 10, 60, 36, 16, start_bg, g_theme_light ? 245 : 235);
  gfx_draw_round_rect(dx + 12, dy + 10, 60, 36, 16, ACCENT_CYAN);
  gfx_draw_text(dx + 30, dy + 20, "L", TEXT, start_bg);

  /* Only show OPENED apps in the dock (including minimized). */
  static const char* dock_names[WIN_MAX] = { "Term", "Demo", "Note", "Files", "Trash" };
  int list[WIN_MAX];
  int n = dock_open_list(list);

  for (int s = 0; s < n; s++) {
    int i = list[s];
    int bx, by, bw, bh;
    app_slot_rect(s, n, &bx, &by, &bw, &bh);

    int is_active = (i == active) && wins[i].visible;
    int is_min    = wins[i].opened && !wins[i].visible;

    uint32_t bg = is_active ? (g_theme_light ? 0x00DCE9FFu : 0x00152345u)
                            : (g_theme_light ? 0x00EEF6FFu : 0x00101A31u);
    uint32_t bd = is_active ? ACCENT_CYAN : WIN_BORDER;

    /* Slightly dim minimized apps */
    uint8_t alpha = g_theme_light ? 245 : 235;
    if (is_min) alpha = (uint8_t)(alpha - 40);

    gfx_fill_round_rect_a(bx, by, bw, bh, 14, bg, alpha);
    gfx_draw_round_rect(bx, by, bw, bh, 14, bd);

    /* label */
    const char* name = dock_names[i];
    gfx_draw_text(bx + 12, by + 11, name, is_active ? TEXT : TEXT_DIM, bg);

    /* running indicator */
    if (wins[i].opened) {
      uint32_t ind = is_active ? ACCENT_PURPLE : (is_min ? WIN_BORDER : WIN_EDGE_DIM);
      gfx_fill_round_rect(bx + bw / 2 - 10, by + bh - 6, 20, 3, 2, ind);
    }
  }

  /* Right controls */
  int xs, xt, xc, xp, yy, hh;
  dock_right_boxes(&xc, &xt, &xs, &xp, &yy, &hh);

  char ts[9];
  fmt_time(ts);
  gfx_fill_round_rect_a(xc, yy, 88, hh, 16, PANEL_BG, g_theme_light ? 245 : 240);
  gfx_draw_round_rect(xc, yy, 88, hh, 16, ACCENT_PURPLE);
  gfx_draw_text(xc + 18, yy + 10, ts, TEXT, PANEL_BG);

  uint32_t tbg = PANEL_BG;
  gfx_fill_round_rect_a(xt, yy, 52, hh, 16, tbg, g_theme_light ? 245 : 240);
  gfx_draw_round_rect(xt, yy, 52, hh, 16, ACCENT_CYAN);
  gfx_draw_text(xt + 20, yy + 10, g_theme_light ? "D" : "L", TEXT, tbg);

  uint32_t db = show_desktop ? (g_theme_light ? 0x00DCE9FFu : 0x00152345u)
                            : (g_theme_light ? 0x00EEF6FFu : 0x00111B34u);
  gfx_fill_round_rect_a(xs, yy, 52, hh, 16, db, g_theme_light ? 245 : 240);
  gfx_draw_round_rect(xs, yy, 52, hh, 16, ACCENT_CYAN);
  gfx_draw_text(xs + 14, yy + 10, "[]", TEXT, db);

  /* Power button */
  uint32_t pb = g_theme_light ? 0x00FFE9F3u : 0x00110A1Du;
  gfx_fill_round_rect_a(xp, yy, 52, hh, 16, pb, g_theme_light ? 245 : 240);
  gfx_draw_round_rect(xp, yy, 52, hh, 16, ACCENT_PINK);

  int pcx = xp + 26;
  int pcy = yy + 18;
  gfx_draw_round_rect(pcx - 9, pcy - 7, 18, 18, 9, ACCENT_PINK);
  gfx_fill_rect(pcx - 1, pcy - 14, 2, 10, ACCENT_PINK);

  if (start_open) {
    int mw = 320;
    int mh = 244;
    int mx0 = dx;
    int my0 = dy - mh - 12;

    gfx_fill_round_rect_a(mx0 + 3, my0 + 6, mw, mh, 20, SHADOW1, g_theme_light ? 90 : 160);
    gfx_fill_round_rect_vgrad(mx0, my0, mw, mh, 20, DOCK_BG, PANEL_BG);
    gfx_draw_round_rect(mx0, my0, mw, mh, 20, ACCENT_PURPLE);
    gfx_draw_text_ta(mx0 + 19, my0 + 19, "Launch", 0x00000000u, g_theme_light ? 80 : 120);
    gfx_draw_text_t(mx0 + 18, my0 + 18, "Launch", TEXT);

    const char* items[6] = {
      "Open Terminal",
      "Open Demo",
      "Open Notepad",
      "Open My Computer",
      "Open Trash",
      "Close Menu"
    };

    start_hot = start_menu_hit_item(mouse_x, mouse_y);

    for (int i = 0; i < 6; i++) {
      int iy = my0 + 14 + i * 36;
      uint32_t ibg = (start_hot == i) ? (g_theme_light ? 0x00DCE9FFu : 0x00152345u)
                                     : (g_theme_light ? 0x00EEF6FFu : 0x0010192Eu);
      uint32_t ibd = (start_hot == i) ? ACCENT_CYAN : WIN_BORDER;
      gfx_fill_round_rect_a(mx0 + 12, iy, mw - 24, 28, 12, ibg, g_theme_light ? 250 : 236);
      gfx_draw_round_rect(mx0 + 12, iy, mw - 24, 28, 12, ibd);
      gfx_draw_text(mx0 + 24, iy + 7, items[i], TEXT, ibg);
    }
  }
}

static void draw_desktop_menu(void) {
  if (!desktop_menu_open) return;

  int mw = 260;
  int mh = 5 * 30 + 12;
  int mx0 = desktop_menu_x;
  int my0 = desktop_menu_y;
  if (mx0 + mw > gfx_w()) mx0 = gfx_w() - mw;
  if (my0 + mh > work_h()) my0 = work_h() - mh;

  desktop_menu_hot = desktop_menu_hit_item(mouse_x, mouse_y);

  gfx_fill_round_rect_a(mx0 + 3, my0 + 6, mw, mh, 18, SHADOW1, g_theme_light ? 90 : 160);
  gfx_fill_round_rect_vgrad(mx0, my0, mw, mh, 18, DOCK_BG, PANEL_BG);
  gfx_draw_round_rect(mx0, my0, mw, mh, 18, ACCENT_PURPLE);

  const char* items[5] = { "Refresh", "New Folder", "Open My Computer", "Show Desktop", "Toggle Theme" };

  for (int i = 0; i < 5; i++) {
    int iy = my0 + 6 + i * 30;
    uint32_t bg = (desktop_menu_hot == i) ? (g_theme_light ? 0x00DCE9FFu : 0x00152345u)
                                         : (g_theme_light ? 0x00EEF6FFu : 0x0010192Eu);
    uint32_t bd = (desktop_menu_hot == i) ? ACCENT_CYAN : WIN_BORDER;
    gfx_fill_round_rect_a(mx0 + 8, iy, mw - 16, 24, 10, bg, g_theme_light ? 250 : 236);
    gfx_draw_round_rect(mx0 + 8, iy, mw - 16, 24, 10, bd);
    gfx_draw_text(mx0 + 18, iy + 4, items[i], TEXT, bg);
  }
}

/* ---------- Cursor ---------- */

void wm_get_cursor_rect(int* x, int* y, int* w, int* h) {
  if (x) *x = mouse_x - 2;
  if (y) *y = mouse_y - 2;
  if (w) *w = 18;
  if (h) *h = 24;
}

void wm_draw_cursor(void) {
  static const uint16_t mask[16] = {
    0b1000000000,0b1100000000,0b1110000000,0b1111000000,
    0b1111100000,0b1111110000,0b1111111000,0b1111111100,
    0b1111111110,0b1111110000,0b1110110000,0b1100011000,
    0b1000001100,0b0000000110,0b0000000011,0b0000000001,
  };

  for (int yy = 0; yy < 16; yy++) {
    uint16_t m = mask[yy];
    for (int xx = 0; xx < 10; xx++) {
      if (!(m & (1u << (9 - xx)))) continue;
      gfx_fill_rect_direct(mouse_x + xx + 1, mouse_y + yy + 1, 1, 1, ACCENT_PURPLE);
      gfx_fill_rect_direct(mouse_x + xx, mouse_y + yy, 1, 1, ACCENT_CYAN);
      if (xx == 0 || yy == 0) gfx_fill_rect_direct(mouse_x + xx, mouse_y + yy, 1, 1, 0x00FFFFFFu);
    }
  }
}

void wm_get_mouse(int* x, int* y) { if (x) *x = mouse_x; if (y) *y = mouse_y; }

uint32_t wm_take_redraw_flags(void) {
  uint32_t f = g_redraw_flags;
  g_redraw_flags = WM_RF_NONE;
  return f;
}

/* ---------- Render ---------- */

void wm_render_full(void) {
  gfx_reset_clip();

  draw_wallpaper();
  draw_desktop_icons();
  draw_dock();
  draw_desktop_menu();

  for (int zi = 0; zi < WIN_MAX; zi++) {
    int i = zorder[zi];
    window_t* w = &wins[i];
    if (!w->visible) continue;

    draw_window_frame(w, i == active);

    /* ✅ preview frame while dragging/resizing */
    if (w->dragging || w->resizing) {
      draw_preview_client(w);
      continue;
    }

    if (w->type == WIN_TERM) draw_terminal_content(w);
    else if (w->type == WIN_DEMO) draw_demo_content(w);
    else if (w->type == WIN_NOTE) draw_notepad_content(w);
    else if (w->type == WIN_EXPL || w->type == WIN_TRASH) draw_list_window_content(w);
  }
}

void wm_paint_rect(int x, int y, int w, int h) {
  gfx_set_clip(x, y, w, h);
  wm_render_full();
  gfx_reset_clip();
}

/* ---------- Init ---------- */

void wm_init(const boot_info_t* bi) {
  (void)bi;

  term_set_theme(0); /* default dark */

  int client_w = term_cols() * 8;
  int client_h = term_rows() * gfx_font_h();
  int ww = client_w + 40;
  int hh = client_h + title_h() + 40;

  wins[WM_WIN_TERM].x = 96; wins[WM_WIN_TERM].y = 72;
  wins[WM_WIN_TERM].w = ww;  wins[WM_WIN_TERM].h = hh;
  wins[WM_WIN_TERM].title = "Terminal";
  wins[WM_WIN_TERM].visible = 1;
  wins[WM_WIN_TERM].opened  = 1;
  wins[WM_WIN_TERM].type = WIN_TERM;
  wins[WM_WIN_TERM].state = WS_NORMAL;

  wins[WM_WIN_DEMO].x = 740; wins[WM_WIN_DEMO].y = 96;
  wins[WM_WIN_DEMO].w = 380; wins[WM_WIN_DEMO].h = 260;
  wins[WM_WIN_DEMO].title = "Demo";
  wins[WM_WIN_DEMO].visible = 1;
  wins[WM_WIN_DEMO].type = WIN_DEMO;
  wins[WM_WIN_DEMO].state = WS_NORMAL;

  wins[WM_WIN_NOTE].x = 230; wins[WM_WIN_NOTE].y = 138;
  wins[WM_WIN_NOTE].w = 620; wins[WM_WIN_NOTE].h = 420;
  wins[WM_WIN_NOTE].title = "Notepad";
  wins[WM_WIN_NOTE].visible = 1;
  wins[WM_WIN_NOTE].type = WIN_NOTE;
  wins[WM_WIN_NOTE].state = WS_NORMAL;

  wins[WM_WIN_EXPL].x = 360; wins[WM_WIN_EXPL].y = 120;
  wins[WM_WIN_EXPL].w = 480; wins[WM_WIN_EXPL].h = 360;
  wins[WM_WIN_EXPL].title = "My Computer";
  wins[WM_WIN_EXPL].visible = 0;
  wins[WM_WIN_EXPL].opened  = 0;
  wins[WM_WIN_EXPL].type = WIN_EXPL;
  wins[WM_WIN_EXPL].state = WS_NORMAL;

  wins[WM_WIN_TRASH].x = 420; wins[WM_WIN_TRASH].y = 160;
  wins[WM_WIN_TRASH].w = 360; wins[WM_WIN_TRASH].h = 260;
  wins[WM_WIN_TRASH].title = "Trash";
  wins[WM_WIN_TRASH].visible = 0;
  wins[WM_WIN_TRASH].opened  = 0;
  wins[WM_WIN_TRASH].type = WIN_TRASH;
  wins[WM_WIN_TRASH].state = WS_NORMAL;

  wins[WM_WIN_DEMO].tb.text[0] = 0;
  wins[WM_WIN_DEMO].tb.len = 0;
  wins[WM_WIN_DEMO].tb.caret = 0;
  wins[WM_WIN_DEMO].tb.focused = 0;

  wins[WM_WIN_NOTE].note[0] = 0;
  wins[WM_WIN_NOTE].note_len = 0;
  wins[WM_WIN_NOTE].note_caret = 0;
  wins[WM_WIN_NOTE].note_scroll = 0;
  wins[WM_WIN_NOTE].note_focused = 1;
  wins[WM_WIN_NOTE].note_path[0] = 0;
  notepad_new_file(&wins[WM_WIN_NOTE]);

  explorer_build_root(&wins[WM_WIN_EXPL]);
  trash_build(&wins[WM_WIN_TRASH]);

  for (int i = 0; i < WIN_MAX; i++) {
    wins[i].dragging = 0;
    wins[i].resizing = 0;
    /* opened tracks “running app”, even if minimized */
    wins[i].opened = wins[i].visible;
    wins[i].sx = wins[i].x; wins[i].sy = wins[i].y;
    wins[i].sw = wins[i].w; wins[i].sh = wins[i].h;
    wins[i].last_title_click_ts = 0;
    wins[i].last_title_click_x = 0;
    wins[i].last_title_click_y = 0;
    clamp_window(&wins[i]);
  }

  active = WM_WIN_TERM;
  bring_to_front(WM_WIN_TERM);

  show_desktop = 0;
  show_desktop_mask = 0;

  icon_hot = -1;
  icon_sel = 0;

  last_icon_click_ms = 0;
  last_icon_click_idx = -1;
  last_icon_click_x = 0;
  last_icon_click_y = 0;

  icon_dragging = 0;
  icon_drag_idx = -1;
  icon_drag_moved = 0;
  desktop_focus = 1;

  desktop_menu_open = 0;
  desktop_menu_x = 0;
  desktop_menu_y = 0;
  desktop_menu_hot = -1;

  g_redraw_flags = WM_RF_FULL;
  dirty_valid = 0;
}

/* ---------- Resize/Drag helpers ---------- */

static void start_resize(window_t* w, hit_t ht) {
  w->resizing = 1;
  w->resize_hit = ht;
  w->rx0 = w->x; w->ry0 = w->y; w->rw0 = w->w; w->rh0 = w->h;
  w->mdown_x = mouse_x;
  w->mdown_y = mouse_y;
}

static void apply_resize(window_t* w) {
  int dx = mouse_x - w->mdown_x;
  int dy = mouse_y - w->mdown_y;

  int x = w->rx0, y = w->ry0, ww = w->rw0, hh = w->rh0;

  switch (w->resize_hit) {
    case HT_L:  x += dx; ww -= dx; break;
    case HT_R:  ww += dx; break;
    case HT_T:  y += dy; hh -= dy; break;
    case HT_B:  hh += dy; break;
    case HT_TL: x += dx; ww -= dx; y += dy; hh -= dy; break;
    case HT_TR: ww += dx; y += dy; hh -= dy; break;
    case HT_BL: x += dx; ww -= dx; hh += dy; break;
    case HT_BR: ww += dx; hh += dy; break;
    default: break;
  }

  w->x = x; w->y = y; w->w = ww; w->h = hh;
  w->state = WS_NORMAL;
  clamp_window(w);
}

static int near_left_edge(void)  { return mouse_x <= 2; }
static int near_right_edge(void) { return mouse_x >= work_w() - 3; }
static int near_top_edge(void)   { return mouse_y <= 2; }

/* ---------- Event handling ---------- */

int wm_handle_event(const event_t* ev) {
  if (!ev) return 0;

  /* Auto-refresh explorer when filesystem changes (mkdir/touch/write/rm). */
  if (ev->type == EV_TICK) {
    if (wins[WM_WIN_EXPL].visible) {
      uint32_t gen = fat32_generation();
      if (gen != wins[WM_WIN_EXPL].fs_gen_seen) {
        __wm_explorer_refresh(&wins[WM_WIN_EXPL], 1);
        g_redraw_flags |= WM_RF_EXPL;
      }
    }
    return 0;
  }

  if (ev->type == EV_MOUSE) {
    mouse_x = ev->x;
    mouse_y = ev->y;

    int prev_hot = icon_hot;
    icon_hot = hit_icon(mouse_x, mouse_y);
    if (prev_hot != icon_hot) g_redraw_flags |= WM_RF_FULL;

    int prev_dm_hot = desktop_menu_hot;
    desktop_menu_hot = desktop_menu_hit_item(mouse_x, mouse_y);
    if (prev_dm_hot != desktop_menu_hot && desktop_menu_open) g_redraw_flags |= WM_RF_FULL;

    uint8_t b = (uint8_t)ev->code;
    int l_prev = (last_buttons & 1) != 0;
    int r_prev = (last_buttons & 2) != 0;
    int l_now  = (b & 1) != 0;
    int r_now  = (b & 2) != 0;
    int l_pressed  = (!l_prev && l_now);
    int l_released = (l_prev && !l_now);
    int r_pressed  = (!r_prev && r_now);
    last_buttons = b;

    /* icon dragging priority */
    if (icon_dragging) {
      int idx = icon_drag_idx;

      if (l_now) {
        int dm = iabs(mouse_x - icon_drag_start_mx) + iabs(mouse_y - icon_drag_start_my);
        if (dm > 3) icon_drag_moved = 1;

        if (icon_drag_moved && idx >= 0 && idx < ICON_COUNT) {
          int ox, oy, ow, oh;
          icon_rect(idx, &ox, &oy, &ow, &oh);

          icons[idx].x = icon_drag_start_x + (mouse_x - icon_drag_start_mx);
          icons[idx].y = icon_drag_start_y + (mouse_y - icon_drag_start_my);

          if (icons[idx].x < 28) icons[idx].x = 28;
          if (icons[idx].y < 24) icons[idx].y = 24;
          if (icons[idx].x > gfx_w() - 140) icons[idx].x = gfx_w() - 140;
          if (icons[idx].y > work_h() - 160) icons[idx].y = work_h() - 160;

          int nx, ny, nw, nh;
          icon_rect(idx, &nx, &ny, &nw, &nh);

          set_dirty_union(ox, oy, ow, oh);
          set_dirty_union(nx, ny, nw, nh);
          g_redraw_flags |= WM_RF_DIRTY;
          return 1;
        }
      }

      if (l_released) {
        if (idx >= 0 && idx < ICON_COUNT) {
          if (icon_drag_moved) {
            int ox, oy, ow, oh;
            icon_rect(idx, &ox, &oy, &ow, &oh);
            icon_snap_to_grid(&icons[idx].x, &icons[idx].y);
            int nx, ny, nw, nh;
            icon_rect(idx, &nx, &ny, &nw, &nh);
            set_dirty_union(ox, oy, ow, oh);
            set_dirty_union(nx, ny, nw, nh);
            g_redraw_flags |= WM_RF_DIRTY;
          } else {
            uint64_t now_ms = timer_uptime_ms();
            const uint64_t DBL_MS = 450;

            if (last_icon_click_ms &&
                (now_ms - last_icon_click_ms) <= DBL_MS &&
                last_icon_click_idx == idx &&
                iabs(mouse_x - last_icon_click_x) <= 6 &&
                iabs(mouse_y - last_icon_click_y) <= 6) {

              open_app_window(icons[idx].win_idx);
              last_icon_click_ms = 0;
              last_icon_click_idx = -1;
            } else {
              last_icon_click_ms = now_ms;
              last_icon_click_idx = idx;
              last_icon_click_x = mouse_x;
              last_icon_click_y = mouse_y;
            }
          }
        }

        icon_dragging = 0;
        icon_drag_idx = -1;
        icon_drag_moved = 0;
        return 1;
      }
    }

    int oldx=0, oldy=0, oldw=0, oldh=0;
    wm_get_paint_rect(active, &oldx,&oldy,&oldw,&oldh);

    if (wins[active].resizing && l_now) {
      apply_resize(&wins[active]);
      int nx,ny,nw,nh;
      wm_get_paint_rect(active, &nx,&ny,&nw,&nh);
      set_dirty_union(oldx,oldy,oldw,oldh);
      set_dirty_union(nx,ny,nw,nh);
      g_redraw_flags |= WM_RF_DIRTY;
      desktop_focus = 0;
      return 1;
    }
    if (wins[active].resizing && l_released) { wins[active].resizing = 0; desktop_focus = 0; return 1; }

    if (wins[active].dragging && l_now) {
      wins[active].x = mouse_x - wins[active].drag_off_x;
      wins[active].y = mouse_y - wins[active].drag_off_y;
      wins[active].state = WS_NORMAL;
      clamp_window(&wins[active]);

      int nx,ny,nw,nh;
      wm_get_paint_rect(active, &nx,&ny,&nw,&nh);
      set_dirty_union(oldx,oldy,oldw,oldh);
      set_dirty_union(nx,ny,nw,nh);
      g_redraw_flags |= WM_RF_DIRTY;
      desktop_focus = 0;
      return 1;
    }

    if (wins[active].dragging && l_released) {
      wins[active].dragging = 0;
      int px,py,pw,ph;
      wm_get_paint_rect(active, &px,&py,&pw,&ph);

      if (near_top_edge()) toggle_max_restore(&wins[active]);
      else if (near_left_edge()) snap_left(&wins[active]);
      else if (near_right_edge()) snap_right(&wins[active]);

      int nx,ny,nw,nh;
      wm_get_paint_rect(active, &nx,&ny,&nw,&nh);

      set_dirty_union(px,py,pw,ph);
      set_dirty_union(nx,ny,nw,nh);
      g_redraw_flags |= WM_RF_DIRTY | WM_RF_TASKBAR;
      desktop_focus = 0;
      return 1;
    }

    if (r_pressed) {
      if (!topmost_at(mouse_x, mouse_y) && mouse_y < work_h()) {
        desktop_menu_open = 1;
        desktop_menu_x = mouse_x;
        desktop_menu_y = mouse_y;
        g_redraw_flags |= WM_RF_FULL;
        desktop_focus = 1;
        return 1;
      }
    }

    if (l_pressed) {
      if (desktop_menu_open) {
        int item = desktop_menu_hit_item(mouse_x, mouse_y);
        if (item == 0) { desktop_menu_open = 0; g_redraw_flags |= WM_RF_FULL; desktop_focus = 1; return 1; }
        if (item == 1) { desktop_menu_open = 0; g_redraw_flags |= WM_RF_FULL; desktop_focus = 1; return 1; }
        if (item == 2) { desktop_menu_open = 0; open_app_window(WM_WIN_EXPL); return 1; }
        if (item == 3) { desktop_menu_open = 0; show_desktop_toggle(); return 1; }
        if (item == 4) { desktop_menu_open = 0; toggle_theme(); return 1; }
        desktop_menu_open = 0; g_redraw_flags |= WM_RF_FULL; desktop_focus = 1; return 1;
      }

      if (theme_hit(mouse_x, mouse_y)) { toggle_theme(); desktop_focus = 0; return 1; }
      if (show_desktop_hit(mouse_x, mouse_y)) { show_desktop_toggle(); return 1; }
      if (power_hit(mouse_x, mouse_y)) { system_shutdown(); return 1; }

      if (start_button_hit(mouse_x, mouse_y)) {
        start_open = !start_open;
        g_redraw_flags |= WM_RF_FULL;
        desktop_focus = 0;
        return 1;
      }

      int ic = hit_icon(mouse_x, mouse_y);
      if (ic >= 0) {
        desktop_focus = 1;

        int prev_sel = icon_sel;
        icon_sel = ic;
        if (prev_sel != icon_sel) g_redraw_flags |= WM_RF_FULL;

        icon_dragging = 1;
        icon_drag_idx = ic;
        icon_drag_start_mx = mouse_x;
        icon_drag_start_my = mouse_y;
        icon_drag_start_x  = icons[ic].x;
        icon_drag_start_y  = icons[ic].y;
        icon_drag_moved = 0;

        return 1;
      }

      int hit = hit_app_button(mouse_x, mouse_y);
      if (hit >= 0) {
        desktop_focus = 0;
        show_desktop = 0;

        /* If not opened yet, open it. */
        if (!wins[hit].opened) {
          open_app_window(hit);
          return 1;
        }

        /* If it's minimized/hidden, restore it. */
        if (!wins[hit].visible) {
          wins[hit].visible = 1;
          active = hit;
          bring_to_front(hit);
          g_redraw_flags |= WM_RF_FULL;
          return 1;
        }

        /* If it's already focused, minimize it (keep opened). */
        if (active == hit) {
          wins[hit].visible = 0;
          active = pick_next_visible(hit);
          g_redraw_flags |= WM_RF_FULL;
          return 1;
        }

        /* Otherwise focus it. */
        active = hit;
        bring_to_front(hit);
        g_redraw_flags |= WM_RF_FULL;
        return 1;
      }

      if (start_open) {
        int item = start_menu_hit_item(mouse_x, mouse_y);
        if (item == 0) { start_open = 0; open_app_window(WM_WIN_TERM); return 1; }
        if (item == 1) { start_open = 0; open_app_window(WM_WIN_DEMO); return 1; }
        if (item == 2) { start_open = 0; open_app_window(WM_WIN_NOTE); return 1; }
        if (item == 3) { start_open = 0; open_app_window(WM_WIN_EXPL); return 1; }
        if (item == 4) { start_open = 0; open_app_window(WM_WIN_TRASH); return 1; }
        if (item == 5) { start_open = 0; g_redraw_flags |= WM_RF_FULL; return 1; }
        if (item < 0) { start_open = 0; g_redraw_flags |= WM_RF_FULL; }
      }

      int w_hit = topmost_at(mouse_x, mouse_y);
      if (w_hit >= 0) {
        desktop_focus = 0;

        int prev_active = active;
        active = w_hit;
        bring_to_front(w_hit);

        window_t* w = &wins[w_hit];
        hit_t ht = hit_test(w, mouse_x, mouse_y);

        if (prev_active != active) {
          int ax,ay,aw,ah, bx,by,bw,bh;
          wm_get_paint_rect(prev_active, &ax,&ay,&aw,&ah);
          wm_get_paint_rect(active, &bx,&by,&bw,&bh);
          set_dirty_union(ax,ay,aw,ah);
          set_dirty_union(bx,by,bw,bh);
          g_redraw_flags |= WM_RF_DIRTY | WM_RF_TASKBAR;
        }

        if (ht == HT_CLOSE) {
          w->visible = 0;
          w->opened = 0;
          if (w_hit == WM_WIN_DEMO) wins[WM_WIN_DEMO].tb.focused = 0;
          if (w_hit == WM_WIN_NOTE) wins[WM_WIN_NOTE].note_focused = 0;
          active = pick_next_visible(w_hit);
          g_redraw_flags |= WM_RF_FULL;
          return 1;
        }

        if (ht == HT_MIN) {
          w->visible = 0;
          active = pick_next_visible(w_hit);
          g_redraw_flags |= WM_RF_FULL;
          return 1;
        }

        if (ht == HT_MAX) {
          int bx,by,bw,bh;
          wm_get_paint_rect(w_hit, &bx,&by,&bw,&bh);
          toggle_max_restore(w);
          int nx,ny,nw,nh;
          wm_get_paint_rect(w_hit, &nx,&ny,&nw,&nh);
          set_dirty_union(bx,by,bw,bh);
          set_dirty_union(nx,ny,nw,nh);
          g_redraw_flags |= WM_RF_DIRTY | WM_RF_TASKBAR;
          return 1;
        }

        if (ht == HT_L || ht == HT_R || ht == HT_T || ht == HT_B ||
            ht == HT_TL || ht == HT_TR || ht == HT_BL || ht == HT_BR) {
          start_resize(w, ht);
          wins[WM_WIN_DEMO].tb.focused = 0;
          wins[WM_WIN_NOTE].note_focused = 0;
          return 1;
        }

        if (ht == HT_TITLE) {
          const uint64_t now_ms = timer_uptime_ms();
          const uint64_t DBL_MS = 450;

          if (w->last_title_click_ts &&
              (now_ms - w->last_title_click_ts) <= DBL_MS &&
              iabs(mouse_x - w->last_title_click_x) <= 6 &&
              iabs(mouse_y - w->last_title_click_y) <= 6) {

            int bx,by,bw,bh;
            wm_get_paint_rect(w_hit, &bx,&by,&bw,&bh);

            toggle_max_restore(w);

            int nx,ny,nw,nh;
            wm_get_paint_rect(w_hit, &nx,&ny,&nw,&nh);

            set_dirty_union(bx,by,bw,bh);
            set_dirty_union(nx,ny,nw,nh);
            g_redraw_flags |= WM_RF_DIRTY | WM_RF_TASKBAR;

            w->last_title_click_ts = 0;
            return 1;
          }

          w->last_title_click_ts = now_ms;
          w->last_title_click_x = mouse_x;
          w->last_title_click_y = mouse_y;

          if (w->state != WS_NORMAL) restore_to_saved(w);

          w->dragging = 1;
          w->drag_off_x = mouse_x - w->x;
          w->drag_off_y = mouse_y - w->y;
          wins[WM_WIN_DEMO].tb.focused = 0;
          if (w_hit != WM_WIN_NOTE) wins[WM_WIN_NOTE].note_focused = 0;
          return 1;
        }

        if (w->type == WIN_DEMO) {
          int changed = 0;
          changed |= ui_textbox_mouse(&w->tb, mouse_x, mouse_y, l_pressed);
          changed |= ui_button_mouse(&w->btn, mouse_x, mouse_y, l_now, l_pressed, l_released);
          if (changed) g_redraw_flags |= WM_RF_DEMO;
          return 1;
        }

        if (w->type == WIN_NOTE) {
          int cx, cy, cw, ch;
          notepad_client_rect(w, &cx, &cy, &cw, &ch);
          if (in_rect(mouse_x, mouse_y, cx, cy, cw, ch)) {
            w->note_focused = 1;
            wins[WM_WIN_DEMO].tb.focused = 0;

            int fh = gfx_font_h();
            int row = (mouse_y - (cy + 4)) / fh;
            if (row < 0) row = 0;
            int col = (mouse_x - (cx + 6)) / 8;
            if (col < 0) col = 0;

            int target_line = w->note_scroll + row;
            w->note_caret = notepad_index_from_line_col(w, target_line, col);

            g_redraw_flags |= WM_RF_NOTE;
            return 1;
          }
          return 1;
        }

        if (w->type == WIN_EXPL || w->type == WIN_TRASH) {
          int cx, cy, cw, ch;
          list_client_rect(w, &cx, &cy, &cw, &ch);
          if (in_rect(mouse_x, mouse_y, cx, cy, cw, ch)) {
            int fh = gfx_font_h();
            int list_y0 = cy + gfx_font_h() + 24;
            int row_h = fh + 8;
            int idx = w->list_scroll + ((mouse_y - list_y0) / row_h);
            if (idx >= 0 && idx < w->item_count) {
              if (w->list_sel == idx) explorer_open_selected(w);
              else { w->list_sel = idx; g_redraw_flags |= (w->type == WIN_EXPL) ? WM_RF_EXPL : WM_RF_TRASH; }
            }
            return 1;
          }
          return 1;
        }

        wins[WM_WIN_DEMO].tb.focused = 0;
        wins[WM_WIN_NOTE].note_focused = 0;
        return 0;
      }

      /* click empty desktop */
      desktop_focus = 1;
      start_open = 0;
      g_redraw_flags |= WM_RF_FULL;
      return 1;
    }

    return 1;
  }

  /* EV_CHAR */
  if (ev->type == EV_CHAR) {
    char c = (char)ev->code;

    if (desktop_focus && (c == '\n' || c == '\r')) {
      if (icon_sel < 0) icon_sel = 0;
      if (icon_sel >= ICON_COUNT) icon_sel = ICON_COUNT - 1;
      open_app_window(icons[icon_sel].win_idx);
      return 1;
    }

    if ((wins[WM_WIN_EXPL].visible && active == WM_WIN_EXPL) ||
        (wins[WM_WIN_TRASH].visible && active == WM_WIN_TRASH)) {
      window_t* w = &wins[active];
      if (c == '\n' || c == '\r') {
        explorer_open_selected(w);
        g_redraw_flags |= (w->type == WIN_EXPL) ? WM_RF_EXPL : WM_RF_TRASH;
        return 1;
      }
      return 1;
    }

    if (wins[WM_WIN_NOTE].visible && active == WM_WIN_NOTE && wins[WM_WIN_NOTE].note_focused) {
      /* shortcuts: Ctrl+S save, Ctrl+N new, Ctrl+O open explorer */
      if (ev->mods & MOD_CTRL) {
        if (c == 's' || c == 'S') { notepad_save_file(&wins[WM_WIN_NOTE]); g_redraw_flags |= WM_RF_NOTE; return 1; }
        if (c == 'n' || c == 'N') { notepad_new_file(&wins[WM_WIN_NOTE]); g_redraw_flags |= WM_RF_NOTE; return 1; }
        if (c == 'o' || c == 'O') { open_app_window(WM_WIN_EXPL); g_redraw_flags |= WM_RF_FULL; return 1; }
      }

      if (c == '\b') notepad_backspace(&wins[WM_WIN_NOTE]);
      else notepad_insert(&wins[WM_WIN_NOTE], c);
      g_redraw_flags |= WM_RF_NOTE;
      return 1;
    }

    if (wins[WM_WIN_DEMO].visible && wins[WM_WIN_DEMO].tb.focused) {
      ui_textbox_char(&wins[WM_WIN_DEMO].tb, c);
      g_redraw_flags |= WM_RF_DEMO;
      return 1;
    }

    return 0;
  }

  /* EV_KEY */
  if (ev->type == EV_KEY) {
    if (wins[WM_WIN_NOTE].visible && active == WM_WIN_NOTE && wins[WM_WIN_NOTE].note_focused) {
      key_t k = (key_t)ev->code;
      if (k == KEY_LEFT)  { notepad_move_left(&wins[WM_WIN_NOTE]); g_redraw_flags |= WM_RF_NOTE; return 1; }
      if (k == KEY_RIGHT) { notepad_move_right(&wins[WM_WIN_NOTE]); g_redraw_flags |= WM_RF_NOTE; return 1; }
      if (k == KEY_DELETE){ notepad_delete(&wins[WM_WIN_NOTE]); g_redraw_flags |= WM_RF_NOTE; return 1; }
      return 1;
    }

    if ((wins[WM_WIN_EXPL].visible && active == WM_WIN_EXPL) ||
        (wins[WM_WIN_TRASH].visible && active == WM_WIN_TRASH)) {
      window_t* w = &wins[active];
      key_t k = (key_t)ev->code;

      if (k == KEY_UP) {
        if (w->list_sel > 0) w->list_sel--;
        g_redraw_flags |= (w->type == WIN_EXPL) ? WM_RF_EXPL : WM_RF_TRASH;
        return 1;
      }
      if (k == KEY_DOWN) {
        if (w->list_sel + 1 < w->item_count) w->list_sel++;
        g_redraw_flags |= (w->type == WIN_EXPL) ? WM_RF_EXPL : WM_RF_TRASH;
        return 1;
      }
      return 1;
    }

    if (wins[WM_WIN_DEMO].visible && wins[WM_WIN_DEMO].tb.focused) {
      ui_textbox_key(&wins[WM_WIN_DEMO].tb, (key_t)ev->code);
      g_redraw_flags |= WM_RF_DEMO;
      return 1;
    }

    return 0;
  }

  return 0;
}