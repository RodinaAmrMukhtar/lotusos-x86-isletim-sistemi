#pragma once
#include <stdint.h>

int fat12_mount(uint32_t fs_lba_start);
void fat12_ls_root(void);
int fat12_cat(const char* name83); // "HELLO.TXT"