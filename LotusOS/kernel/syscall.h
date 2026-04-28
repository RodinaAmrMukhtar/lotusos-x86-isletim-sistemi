#pragma once
#include <stdint.h>

struct regs;

/* Syscall numbers */
#define SYS_WRITE 1
#define SYS_EXIT  2
#define SYS_YIELD 3

void syscall_init(void);
void syscall_dispatch(struct regs* r);