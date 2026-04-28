#pragma once
#include <stdint.h>
#include "bootinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

void term_init(const boot_info_t* bi);
void term_clear(void);

void term_putc(char c);
void term_write(const char* s);

int  term_cols(void);
int  term_rows(void);

void term_get_cursor(int* x, int* y);
void term_set_cursor(int x, int y);

/* capture mode for shell */
void term_capture_begin(char* buf, int max);
int  term_capture_end(void);

/* theme: 0 = dark, 1 = light */
void term_set_theme(int light);

/* render into pixel rect */
void term_render(int x_px, int y_px, int w_px, int h_px);

#ifdef __cplusplus
}
#endif