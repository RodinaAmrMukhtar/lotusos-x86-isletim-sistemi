#pragma once
#include <stdint.h>

/*
  regs_t matches the stack layout built by isr.asm.
  If an interrupt happens while in Ring3, the CPU pushes useresp+ss too.
  For Ring0 interrupts those fields are not meaningful.
*/

typedef struct regs {
  uint32_t gs, fs, es, ds;
  uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
  uint32_t int_no, err_code;
  uint32_t eip, cs, eflags;
  uint32_t useresp, ss; /* only valid if (cs & 3) == 3 */
} regs_t;

typedef void (*irq_handler_t)(regs_t* r);

void isr_init(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);