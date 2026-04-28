#pragma once
#include "keys.h"

void shell_init(void);
void shell_on_char(char c);
void shell_on_key(key_t k);