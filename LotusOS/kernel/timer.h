#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void timer_init(uint32_t hz);
uint64_t timer_ticks(void);
uint32_t timer_hz(void);
uint64_t timer_uptime_ms(void);

#ifdef __cplusplus
}
#endif