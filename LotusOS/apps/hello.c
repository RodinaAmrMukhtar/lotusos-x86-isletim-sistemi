#include <stdint.h>

/* Simple Ring3 app using int 0x80 syscalls */

#define SYS_WRITE 1
#define SYS_EXIT  2

static inline int sys_write(const char* buf, uint32_t len) {
  int ret;
  __asm__ volatile(
    "int $0x80"
    : "=a"(ret)
    : "a"(SYS_WRITE), "b"(buf), "c"(len)
    : "memory"
  );
  return ret;
}

static inline void sys_exit(int code) {
  __asm__ volatile(
    "int $0x80"
    :
    : "a"(SYS_EXIT), "b"(code)
    : "memory"
  );
  for (;;) { }
}

void _start(void) {
  const char msg[] = "Hello from HELLO.ELF (Ring3 + sys_write)!\n";
  sys_write(msg, (uint32_t)(sizeof(msg) - 1));
  sys_exit(0);
}