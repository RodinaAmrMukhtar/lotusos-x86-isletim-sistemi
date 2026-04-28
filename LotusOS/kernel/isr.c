#include "isr.h"
#include "terminal.h"
#include "pic.h"
#include "syscall.h"
#include <stdint.h>

static irq_handler_t irq_handlers[16] = {0};

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
  if (irq < 16) irq_handlers[irq] = handler;
}

static const char* exc_msg[32] = {
  "Divide by zero","Debug","NMI","Breakpoint","Overflow","Bound range","Invalid opcode","Device not available",
  "Double fault","Coprocessor segment","Invalid TSS","Segment not present","Stack-segment fault","General protection","Page fault","Reserved",
  "x87 FP exception","Alignment check","Machine check","SIMD FP exception","Virtualization","Control protection","Reserved","Reserved",
  "Reserved","Reserved","Reserved","Reserved","Hypervisor","VMM comm","Security","Reserved"
};

static void term_write_hex32(uint32_t v) {
  const char* hex = "0123456789ABCDEF";
  term_write("0x");
  for (int i = 7; i >= 0; i--) {
    uint8_t n = (uint8_t)((v >> (i * 4)) & 0xF);
    term_putc(hex[n]);
  }
}

static void page_fault_dump(regs_t* r) {
  uint32_t cr2;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

  term_write("\n\nEXCEPTION: Page fault\n");
  term_write("  CR2 (addr) = "); term_write_hex32(cr2); term_putc('\n');
  term_write("  ERR (code) = "); term_write_hex32(r->err_code); term_putc('\n');

  term_write("  Reason: ");
  if (r->err_code & 0x1) term_write("protection-violation ");
  else term_write("non-present ");

  if (r->err_code & 0x2) term_write("write ");
  else term_write("read ");

  if (r->err_code & 0x4) term_write("user ");
  else term_write("kernel ");

  if (r->err_code & 0x8) term_write("reserved-bit ");
  if (r->err_code & 0x10) term_write("instr-fetch ");
  term_putc('\n');

  term_write("  EIP = "); term_write_hex32(r->eip); term_putc('\n');
}

void isr_handler(regs_t* r) {
  /* Syscalls */
  if (r->int_no == 0x80) {
    syscall_dispatch(r);
    return;
  }

  if (r->int_no < 32) {
    if (r->int_no == 14) {
      page_fault_dump(r);
    } else {
      term_write("\n\nEXCEPTION: ");
      term_write(exc_msg[r->int_no]);
      term_putc('\n');
      term_write("  EIP = "); term_write_hex32(r->eip); term_putc('\n');
      term_write("  CS  = "); term_write_hex32(r->cs);  term_putc('\n');
    }

    term_write("System halted.\n");
    for(;;) __asm__ volatile("cli; hlt");
  }
}

void irq_handler(regs_t* r) {
  uint8_t irq = (uint8_t)(r->int_no - 32);
  if (irq < 16 && irq_handlers[irq]) irq_handlers[irq](r);
  pic_send_eoi(irq);
}

void isr_init(void) {
  syscall_init();
}