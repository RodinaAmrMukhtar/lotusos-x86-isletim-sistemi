#pragma once
#include <stdint.h>
#include "bootinfo.h"

/* Simple 32-bit backbuffer renderer + partial present() to VBE framebuffer */

#ifdef __cplusplus
extern "C" {
#endif

int  gfx_init(const boot_info_t* bi);
void gfx_shutdown(void);

int  gfx_w(void);
int  gfx_h(void);
int  gfx_font_h(void);

void gfx_clear(uint32_t rgb);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t rgb);

/* Alpha fill (0..255) */
void gfx_fill_rect_a(int x, int y, int w, int h, uint32_t rgb, uint8_t a);

/* Gradients + rounded primitives */
void gfx_fill_rect_vgrad(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bot_rgb);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint32_t rgb);
void gfx_fill_round_rect_a(int x, int y, int w, int h, int r, uint32_t rgb, uint8_t a);
void gfx_fill_round_rect_vgrad(int x, int y, int w, int h, int r, uint32_t top_rgb, uint32_t bot_rgb);
void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint32_t rgb);

void gfx_draw_char(int x, int y, uint8_t ch, uint32_t fg, uint32_t bg);
void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg);

/* Transparent text: draws only glyph "on" pixels (no background fill). */
void gfx_draw_char_t(int x, int y, uint8_t ch, uint32_t fg);
void gfx_draw_text_t(int x, int y, const char* s, uint32_t fg);

/* Transparent + alpha text (0..255): blends only glyph "on" pixels. */
void gfx_draw_char_ta(int x, int y, uint8_t ch, uint32_t fg, uint8_t a);
void gfx_draw_text_ta(int x, int y, const char* s, uint32_t fg, uint8_t a);

/* Blit ARGB pixels (0xAARRGGBB). src_pitch is in pixels. */
void gfx_blit_argb(int x, int y, int w, int h, const uint32_t* src, int src_pitch);

/* Same as above but treats key_rgb (0x00RRGGBB) as transparent (alpha=0). */
void gfx_blit_argb_key(int x, int y, int w, int h, const uint32_t* src, int src_pitch, uint32_t key_rgb);

/* Clipping: affects drawing into backbuffer */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);

/* Direct-to-framebuffer primitives for cursor/overlay drawing */
void gfx_fill_rect_direct(int x, int y, int w, int h, uint32_t rgb);

/* Present to real framebuffer */
void gfx_present(void);
void gfx_present_rect(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif