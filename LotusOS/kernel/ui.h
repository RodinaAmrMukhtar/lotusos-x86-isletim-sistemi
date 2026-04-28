#pragma once
#include <stdint.h>
#include "keys.h"

typedef void (*ui_button_cb_t)(void* userdata);

typedef struct ui_button {
  int x, y, w, h;
  const char* label;
  int pressed;
  int hot;
  ui_button_cb_t cb;
  void* userdata;
} ui_button_t;

typedef struct ui_textbox {
  int x, y, w, h;
  char text[128];
  int len;
  int caret;
  int focused;
} ui_textbox_t;

#ifdef __cplusplus
extern "C" {
#endif

void ui_button_draw(const ui_button_t* b);
int  ui_button_mouse(ui_button_t* b, int mx, int my, int l_down, int l_pressed, int l_released);

void ui_textbox_draw(const ui_textbox_t* t);
int  ui_textbox_mouse(ui_textbox_t* t, int mx, int my, int l_pressed);
int  ui_textbox_char(ui_textbox_t* t, char c);
int  ui_textbox_key(ui_textbox_t* t, key_t k);

#ifdef __cplusplus
}
#endif
