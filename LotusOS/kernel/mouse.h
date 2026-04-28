#pragma once
#include "bootinfo.h"

void mouse_init(const boot_info_t* bi);
void mouse_get_pos(int* x, int* y);