#pragma once
#include <stdint.h>

typedef void (*task_entry_t)(void*);

void task_init(void);

/* Kernel tasks */
int  task_create(const char* name, task_entry_t entry, void* arg);

/* User tasks (Ring3) */
int  task_create_user(const char* name, uint32_t entry_eip);

/* Called from isr.asm (both IRQ + ISR): given current stack pointer, returns stack pointer to restore */
uint32_t task_maybe_switch(uint32_t cur_esp);

/* Called by syscalls */
void task_exit_current(void);
void task_request_yield(void);

/* Shell utilities */
void task_ps(void);