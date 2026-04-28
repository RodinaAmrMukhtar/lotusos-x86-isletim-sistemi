#pragma once
#include <stdint.h>
#include "bootinfo.h"
#include "events.h"

/* window indices */
#define WM_WIN_TERM   0
#define WM_WIN_DEMO   1
#define WM_WIN_NOTE   2
#define WM_WIN_EXPL   3
#define WM_WIN_TRASH  4

/* redraw flags (returned by wm_take_redraw_flags) */
#define WM_RF_NONE     0u
#define WM_RF_TERM     1u
#define WM_RF_DEMO     2u
#define WM_RF_NOTE     4u
#define WM_RF_EXPL     8u
#define WM_RF_TRASH    16u
#define WM_RF_TASKBAR  32u
#define WM_RF_FULL     64u
#define WM_RF_DIRTY    128u

#ifdef __cplusplus
extern "C" {
#endif

void wm_init(const boot_info_t* bi);

/* returns 1 if event was consumed (don't forward to shell) */
int  wm_handle_event(const event_t* ev);

/* returns flags and clears them */
uint32_t wm_take_redraw_flags(void);

void wm_get_mouse(int* x, int* y);
void wm_get_cursor_rect(int* x, int* y, int* w, int* h);

/* paint helpers */
void wm_render_full(void);                      /* full redraw into backbuffer (no cursor) */
void wm_paint_rect(int x, int y, int w, int h);/* redraw helper (caller presents only rect) */
void wm_draw_cursor(void);                      /* draw cursor directly to framebuffer */

/* returns a good paint rect for the given window (includes shadow) */
void wm_get_paint_rect(int idx, int* x, int* y, int* w, int* h);

/* window move/resize dirty rectangle */
int  wm_take_dirty_rect(int* x, int* y, int* w, int* h);

#ifdef __cplusplus
}
#endif