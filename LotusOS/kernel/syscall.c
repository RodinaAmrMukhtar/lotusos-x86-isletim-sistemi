#include "syscall.h"
#include "isr.h"
#include "terminal.h"
#include "task.h"

static uint32_t safe_u32(uint32_t v, uint32_t max) {
  return (v > max) ? max : v;
}

void syscall_init(void) {
  /* nothing yet */
}

static uint32_t sys_write_impl(const char* buf, uint32_t len) {
  if (!buf) return 0;
  len = safe_u32(len, 4096u);
  for (uint32_t i = 0; i < len; i++) {
    char c = buf[i];
    term_putc(c);
  }
  return len;
}

void syscall_dispatch(struct regs* rr) {
  regs_t* r = (regs_t*)rr;
  uint32_t num = r->eax;

  switch (num) {
    case SYS_WRITE: {
      const char* buf = (const char*)(uintptr_t)r->ebx;
      uint32_t len = r->ecx;
      r->eax = sys_write_impl(buf, len);
      return;
    }
    case SYS_EXIT: {
      int code = (int)r->ebx;
      (void)code;
      task_exit_current();
      r->eax = 0;
      return;
    }
    case SYS_YIELD: {
      task_request_yield();
      r->eax = 0;
      return;
    }
    default:
      r->eax = (uint32_t)-1;
      return;
  }
}