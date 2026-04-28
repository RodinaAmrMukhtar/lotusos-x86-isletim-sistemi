#pragma once
#include <stdint.h>

int elf_load(const uint8_t* image, uint32_t size, uint32_t* out_entry);