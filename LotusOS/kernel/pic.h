#pragma once
#include <stdint.h>

void pic_remap(uint8_t offset1, uint8_t offset2);
void pic_set_masks(uint8_t master_mask, uint8_t slave_mask);
void pic_send_eoi(uint8_t irq);

/* New helpers */
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);
uint16_t pic_get_masks(void); /* low byte = master, high byte = slave */