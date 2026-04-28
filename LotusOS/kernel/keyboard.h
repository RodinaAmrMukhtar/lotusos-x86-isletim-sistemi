#pragma once
#include <stdint.h>
#include "events.h"

void keyboard_init(void);

/* called from kernel loop (on EV_TICK) to generate repeats */
int keyboard_repeat_event(event_t* out);