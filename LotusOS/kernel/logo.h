#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned char leiurus_logo_rgba[];
extern unsigned int  leiurus_logo_rgba_len;

/* MUST match what you generate (we use 256x256) */
#define LEIURUS_LOGO_W 512
#define LEIURUS_LOGO_H 512

#ifdef __cplusplus
}
#endif