#pragma once
#include <stdint.h>

/* GDT + TSS setup for Ring3 + syscalls */

void gdt_init(void);

/* Update the kernel stack used when the CPU transitions from Ring3 -> Ring0 */
void tss_set_esp0(uint32_t esp0);

/* Selectors */
#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_CS   0x1B /* 0x18 | RPL3 */
#define GDT_USER_DS   0x23 /* 0x20 | RPL3 */
#define GDT_TSS_SEL   0x28