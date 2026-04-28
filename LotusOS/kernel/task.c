#include "task.h"
#include "terminal.h"
#include "kheap.h"
#include "isr.h"
#include "gdt.h"
#include "paging.h"
#include "pmm.h"

#include <stdint.h>
#include <stddef.h>

#define TASK_MAX      16
#define KSTACK_SIZE   (16 * 1024)   /* 16 KB per task */
#define QUANTUM_TICKS 5             /* switch every 5 timer ticks */

/* User stack virtual layout: give each slot a small region */
#define USTACK_REGION_SIZE (64 * 1024u)
#define USTACK_TOP_BASE    0xBFF00000u

typedef enum {
  TASK_UNUSED = 0,
  TASK_RUNNING,
  TASK_READY,
  TASK_DEAD
} task_state_t;

typedef enum {
  TASKK_KERNEL = 0,
  TASKK_USER   = 1
} task_kind_t;

typedef struct {
  int id;
  task_state_t state;
  task_kind_t  kind;
  char name[16];

  task_entry_t entry;
  void* arg;

  uint32_t kstack_base;
  uint32_t kstack_top;
  uint32_t esp;            /* saved IRQ-frame pointer (top of regs_t) */

  uint32_t ustack_top;     /* valid for user tasks */
} task_t;

static task_t g_tasks[TASK_MAX];
static int    g_current = 0;
static int    g_next_id = 1;
static int    g_inited  = 0;
static uint32_t g_quantum = 0;
static volatile int g_yield_requested = 0;

static void str_copy_15(char dst[16], const char* src) {
  int i = 0;
  for (; i < 15 && src && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

/* Kernel trampoline runs task entry, then marks dead */
static void __attribute__((noreturn)) task_trampoline(void) {
  task_t* t = &g_tasks[g_current];
  if (t->entry) t->entry(t->arg);
  t->state = TASK_DEAD;
  for (;;) __asm__ volatile("hlt");
}

static const char* state_name(task_state_t s) {
  switch (s) {
    case TASK_RUNNING: return "RUN";
    case TASK_READY:   return "READY";
    case TASK_DEAD:    return "DEAD";
    default:           return "UNUSED";
  }
}

/*
Stack layout expected by isr.asm before it does:
  pop gs/fs/es/ds, popa, add esp, 8, iretd

We need at ESP (top):
  gs fs es ds edi esi ebp esp_dummy ebx edx ecx eax int_no err_code eip cs eflags [useresp ss]
*/

static uint32_t build_initial_esp_kernel(uint32_t stack_top) {
  uint32_t* sp = (uint32_t*)stack_top;

  *(--sp) = 0x00000202u;                          /* eflags (IF=1) */
  *(--sp) = (uint32_t)GDT_KERNEL_CS;              /* cs */
  *(--sp) = (uint32_t)(uintptr_t)task_trampoline; /* eip */

  *(--sp) = 0;  /* err_code */
  *(--sp) = 0;  /* int_no */

  *(--sp) = 0; /* eax */
  *(--sp) = 0; /* ecx */
  *(--sp) = 0; /* edx */
  *(--sp) = 0; /* ebx */
  *(--sp) = 0; /* esp dummy (for popa) */
  *(--sp) = 0; /* ebp */
  *(--sp) = 0; /* esi */
  *(--sp) = 0; /* edi */

  *(--sp) = (uint32_t)GDT_KERNEL_DS; /* ds */
  *(--sp) = (uint32_t)GDT_KERNEL_DS; /* es */
  *(--sp) = (uint32_t)GDT_KERNEL_DS; /* fs */
  *(--sp) = (uint32_t)GDT_KERNEL_DS; /* gs */

  return (uint32_t)(uintptr_t)sp;
}

static uint32_t build_initial_esp_user(uint32_t kstack_top, uint32_t entry_eip, uint32_t user_stack_top) {
  uint32_t* sp = (uint32_t*)kstack_top;

  /* IRET frame for ring3 return */
  *(--sp) = (uint32_t)GDT_USER_DS;     /* ss */
  *(--sp) = user_stack_top;           /* useresp */
  *(--sp) = 0x00000202u;              /* eflags */
  *(--sp) = (uint32_t)GDT_USER_CS;     /* cs */
  *(--sp) = entry_eip;                /* eip */

  *(--sp) = 0;  /* err_code */
  *(--sp) = 0;  /* int_no */

  *(--sp) = 0; /* eax */
  *(--sp) = 0; /* ecx */
  *(--sp) = 0; /* edx */
  *(--sp) = 0; /* ebx */
  *(--sp) = 0; /* esp dummy */
  *(--sp) = 0; /* ebp */
  *(--sp) = 0; /* esi */
  *(--sp) = 0; /* edi */

  /* Pop into segment regs before iret so Ring3 has correct DS/ES/FS/GS */
  *(--sp) = (uint32_t)GDT_USER_DS; /* ds */
  *(--sp) = (uint32_t)GDT_USER_DS; /* es */
  *(--sp) = (uint32_t)GDT_USER_DS; /* fs */
  *(--sp) = (uint32_t)GDT_USER_DS; /* gs */

  return (uint32_t)(uintptr_t)sp;
}

static int alloc_slot(void) {
  for (int i = 0; i < TASK_MAX; i++) {
    if (g_tasks[i].state == TASK_UNUSED || g_tasks[i].state == TASK_DEAD) return i;
  }
  return -1;
}

static void idle_entry(void* arg) {
  (void)arg;
  for (;;) __asm__ volatile("hlt");
}

void task_request_yield(void) {
  g_yield_requested = 1;
}

void task_exit_current(void) {
  g_tasks[g_current].state = TASK_DEAD;
}

void task_init(void) {
  for (int i = 0; i < TASK_MAX; i++) {
    g_tasks[i].id = 0;
    g_tasks[i].state = TASK_UNUSED;
    g_tasks[i].kind = TASKK_KERNEL;
    g_tasks[i].name[0] = 0;
    g_tasks[i].entry = 0;
    g_tasks[i].arg = 0;
    g_tasks[i].kstack_base = 0;
    g_tasks[i].kstack_top = 0;
    g_tasks[i].esp = 0;
    g_tasks[i].ustack_top = 0;
  }

  g_current = 0;
  g_next_id = 1;
  g_quantum = 0;
  g_yield_requested = 0;

  /* Task 0 is the current “shell thread” (already running) */
  g_tasks[0].id = g_next_id++;
  g_tasks[0].state = TASK_RUNNING;
  g_tasks[0].kind = TASKK_KERNEL;
  str_copy_15(g_tasks[0].name, "shell");
  g_tasks[0].esp = 0; /* captured on first timer IRQ */
  /* esp0 for ring3 transitions while shell is current */
  uint32_t esp;
  __asm__ volatile("mov %%esp, %0" : "=r"(esp));
  tss_set_esp0(esp);

  /* Create idle task */
  (void)task_create("idle", idle_entry, 0);

  g_inited = 1;
}

int task_create(const char* name, task_entry_t entry, void* arg) {
  int idx = alloc_slot();
  if (idx < 0) return -1;

  uint32_t base = (uint32_t)(uintptr_t)kmalloc_aligned(KSTACK_SIZE, 16);
  if (!base) return -1;
  uint32_t top = base + KSTACK_SIZE;

  g_tasks[idx].id = g_next_id++;
  g_tasks[idx].state = TASK_READY;
  g_tasks[idx].kind = TASKK_KERNEL;
  str_copy_15(g_tasks[idx].name, name ? name : "task");
  g_tasks[idx].entry = entry;
  g_tasks[idx].arg = arg;
  g_tasks[idx].kstack_base = base;
  g_tasks[idx].kstack_top = top;
  g_tasks[idx].esp = build_initial_esp_kernel(top);

  return g_tasks[idx].id;
}

static uint32_t user_stack_top_for_slot(int idx) {
  /* give each slot a separate region */
  uint32_t region_top = USTACK_TOP_BASE - (uint32_t)idx * USTACK_REGION_SIZE;
  return region_top;
}

static int map_user_stack(uint32_t top) {
  /* map 1 page for now (can expand later) */
  uint32_t page = top - 0x1000u;
  uint32_t frame = pmm_alloc_frame();
  if (!frame) return -1;
  paging_map(page, frame, PAGE_RW | PAGE_USER);

  /* zero it */
  uint8_t* p = (uint8_t*)(uintptr_t)page;
  for (uint32_t i = 0; i < 4096u; i++) p[i] = 0;
  return 0;
}

int task_create_user(const char* name, uint32_t entry_eip) {
  int idx = alloc_slot();
  if (idx < 0) return -1;

  uint32_t kbase = (uint32_t)(uintptr_t)kmalloc_aligned(KSTACK_SIZE, 16);
  if (!kbase) return -1;
  uint32_t ktop = kbase + KSTACK_SIZE;

  uint32_t u_top = user_stack_top_for_slot(idx);
  if (map_user_stack(u_top) != 0) return -1;

  g_tasks[idx].id = g_next_id++;
  g_tasks[idx].state = TASK_READY;
  g_tasks[idx].kind = TASKK_USER;
  str_copy_15(g_tasks[idx].name, name ? name : "app");
  g_tasks[idx].entry = 0;
  g_tasks[idx].arg = 0;
  g_tasks[idx].kstack_base = kbase;
  g_tasks[idx].kstack_top = ktop;
  g_tasks[idx].ustack_top = u_top;
  g_tasks[idx].esp = build_initial_esp_user(ktop, entry_eip, u_top);

  return g_tasks[idx].id;
}

static int pick_next_ready(void) {
  int start = g_current;
  for (int step = 1; step <= TASK_MAX; step++) {
    int idx = (start + step) % TASK_MAX;
    if (g_tasks[idx].state == TASK_READY) return idx;
  }
  return g_current;
}

static uint32_t do_switch(uint32_t cur_esp, int next) {
  /* Save current */
  g_tasks[g_current].esp = cur_esp;
  if (g_tasks[g_current].state == TASK_RUNNING) g_tasks[g_current].state = TASK_READY;

  g_current = next;
  g_tasks[g_current].state = TASK_RUNNING;

  /* Update TSS.esp0 for ring3 -> ring0 transitions */
  if (g_tasks[g_current].kstack_top) {
    tss_set_esp0(g_tasks[g_current].kstack_top);
  }

  if (g_tasks[g_current].esp == 0) return cur_esp;
  return g_tasks[g_current].esp;
}

uint32_t task_maybe_switch(uint32_t cur_esp) {
  if (!g_inited) return cur_esp;

  regs_t* r = (regs_t*)(uintptr_t)cur_esp;

  /* Capture shell esp on first timer tick */
  if (r->int_no == 32 && g_tasks[g_current].esp == 0) {
    g_tasks[g_current].esp = cur_esp;
    return cur_esp;
  }

  /* If current died (e.g. sys_exit), switch immediately */
  if (g_tasks[g_current].state == TASK_DEAD) {
    int next = pick_next_ready();
    if (next == g_current) return cur_esp;
    return do_switch(cur_esp, next);
  }

  /* Voluntary yield */
  if (g_yield_requested) {
    g_yield_requested = 0;
    int next = pick_next_ready();
    if (next == g_current) return cur_esp;
    return do_switch(cur_esp, next);
  }

  /* Only time-slice on timer IRQ (int 32) */
  if (r->int_no != 32) return cur_esp;

  g_quantum++;
  if (g_quantum < QUANTUM_TICKS) return cur_esp;
  g_quantum = 0;

  int next = pick_next_ready();
  if (next == g_current) return cur_esp;
  return do_switch(cur_esp, next);
}

void task_ps(void) {
  term_write("PID  STATE  NAME\n");
  for (int i = 0; i < TASK_MAX; i++) {
    if (g_tasks[i].state == TASK_UNUSED) continue;

    int pid = g_tasks[i].id;
    char buf[16];
    int bi = 0;
    if (pid == 0) buf[bi++] = '0';
    else {
      int n = pid;
      char tmp[16];
      int ti = 0;
      while (n && ti < 15) { tmp[ti++] = (char)('0' + (n % 10)); n /= 10; }
      while (ti--) buf[bi++] = tmp[ti];
    }
    buf[bi] = 0;

    term_write(buf);
    term_write("  ");
    term_write(state_name(g_tasks[i].state));
    term_write("  ");
    term_write(g_tasks[i].name[0] ? g_tasks[i].name : "(noname)");
    term_putc('\n');
  }
}