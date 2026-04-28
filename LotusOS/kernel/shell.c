#include "shell.h"
#include "terminal.h"
#include "timer.h"
#include "io.h"
#include "fat32.h"
#include "vfs.h"
#include "pmm.h"
#include "kheap.h"
#include "task.h"
#include "elf.h"

#include <stdint.h>
#include <stddef.h>

#define LINE_MAX 128
#define HIST_MAX 16
#define PATH_MAX 128

static char line[LINE_MAX];
static size_t len = 0;
static size_t cur = 0;
static size_t drawn = 0;

static int prompt_x = 0, prompt_y = 0;

static char cwd_drive = 'C';
static uint32_t cwd_cluster = 0;
static char cwd_path[PATH_MAX] = "/";

static char hist[HIST_MAX][LINE_MAX];
static int hist_head = 0;
static int hist_count = 0;
static int hist_nav = -1;

static const char* commands[] = {
  "help","clear","cls","echo","reboot","uptime",
  "ls","cat","cd","pwd",
  "mkdir","touch","write","append","rm",
  "mem","mount",
  "ps","spawn","count",
  "exec"
};
static const int commands_n = (int)(sizeof(commands) / sizeof(commands[0]));

static volatile uint32_t g_bg_counter = 0;

static void bg_worker(void* arg) {
  (void)arg;
  for (;;) {
    g_bg_counter++;
    for (volatile int i = 0; i < 200000; i++) { }
  }
}

static int streq(const char* a, const char* b) {
  while (*a && *b) { if (*a != *b) return 0; a++; b++; }
  return *a == 0 && *b == 0;
}
static int is_space(char c) { return c==' ' || c=='\t'; }

static const char* skip_spaces(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static void trim_right_inplace(char* s) {
  size_t n = 0;
  while (s[n]) n++;
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[n - 1] = 0;
    n--;
  }
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

static void term_write_u64(uint64_t n) {
  if (n == 0) { term_putc('0'); return; }
  char buf[32];
  int i = 0;
  while (n > 0 && i < (int)sizeof(buf)) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
  for (i--; i >= 0; i--) term_putc(buf[i]);
}

static void term_write_u32(uint32_t n) {
  char buf[16];
  int i = 0;
  if (n == 0) { term_putc('0'); return; }
  while (n && i < 15) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
  while (i--) term_putc(buf[i]);
}

static void term_write_3digits(uint32_t n) {
  term_putc((char)('0' + ((n / 100) % 10)));
  term_putc((char)('0' + ((n / 10) % 10)));
  term_putc((char)('0' + (n % 10)));
}

static int str_len(const char* s) { int n=0; while (s && s[n]) n++; return n; }

/* quoted token parser */
static const char* parse_token(const char* s, char* out, int out_max) {
  s = skip_spaces(s);
  int i = 0;

  if (*s == '"') {
    s++;
    while (*s && *s != '"' && i < out_max - 1) out[i++] = *s++;
    out[i] = 0;
    if (*s == '"') s++;
    return s;
  }

  while (*s && !is_space(*s) && i < out_max - 1) out[i++] = *s++;
  out[i] = 0;
  return s;
}

static void pn_push(char comps[32][64], int* top, const char* c) {
  if (!c || !c[0]) return;
  if (*top >= 32) return;
  int i = 0;
  while (c[i] && i < 63) { comps[*top][i] = c[i]; i++; }
  comps[*top][i] = 0;
  (*top)++;
}
static void pn_pop(int* top) { if (*top > 0) (*top)--; }

static void path_normalize(const char* base_abs, const char* arg, char out[PATH_MAX]) {
  char comps[32][64];
  int top = 0;

  if (!(arg && (arg[0] == '/' || arg[0] == '\\'))) {
    const char* p = base_abs;
    while (*p) {
      while (is_sep(*p)) p++;
      if (!*p) break;
      char tmp[64];
      int i = 0;
      while (*p && !is_sep(*p) && i < 63) tmp[i++] = *p++;
      tmp[i] = 0;
      if (tmp[0]) pn_push(comps, &top, tmp);
    }
  }

  const char* p = arg;
  while (*p) {
    while (is_sep(*p)) p++;
    if (!*p) break;

    char tmp[64];
    int i = 0;
    while (*p && !is_sep(*p) && i < 63) tmp[i++] = *p++;
    tmp[i] = 0;

    if (streq(tmp, ".") || tmp[0] == 0) continue;
    if (streq(tmp, "..")) { pn_pop(&top); continue; }
    pn_push(comps, &top, tmp);
  }

  int o = 0;
  out[o++] = '/';
  if (top == 0) { out[o] = 0; return; }

  for (int c = 0; c < top; c++) {
    if (c > 0 && o < PATH_MAX - 1) out[o++] = '/';
    for (int j = 0; comps[c][j] && o < PATH_MAX - 1; j++) out[o++] = comps[c][j];
  }
  out[o] = 0;
}

static void prompt(void) {
  term_write("\n");
  term_putc(cwd_drive);
  term_write(":");
  term_write(cwd_path);
  term_write("> ");
  term_get_cursor(&prompt_x, &prompt_y);
  drawn = 0;
}

static void redraw_line(void) {
  int max_visible = 79 - prompt_x;
  if (max_visible < 0) max_visible = 0;

  if (len > (size_t)max_visible) len = (size_t)max_visible;
  if (cur > len) cur = len;

  term_set_cursor(prompt_x, prompt_y);

  for (size_t i = 0; i < len; i++) term_putc(line[i]);
  for (size_t i = len; i < drawn; i++) term_putc(' ');

  drawn = len;
  term_set_cursor(prompt_x + (int)cur, prompt_y);
}

static void hist_push(const char* s) {
  if (!s || !s[0]) return;

  if (hist_count > 0) {
    int last_idx = (hist_head - 1 + HIST_MAX) % HIST_MAX;
    if (streq(hist[last_idx], s)) return;
  }

  size_t i = 0;
  for (; i < LINE_MAX - 1 && s[i]; i++) hist[hist_head][i] = s[i];
  hist[hist_head][i] = 0;

  hist_head = (hist_head + 1) % HIST_MAX;
  if (hist_count < HIST_MAX) hist_count++;
  hist_nav = -1;
}

static const char* hist_get(int nav) {
  int idx = (hist_head - 1 - nav + HIST_MAX) % HIST_MAX;
  return hist[idx];
}

static void set_line_from(const char* s) {
  len = 0;
  cur = 0;
  while (s && s[len] && len < LINE_MAX - 1) { line[len] = s[len]; len++; }
  line[len] = 0;
  cur = len;
  redraw_line();
}

static void cmd_help(void) {
  term_write("Commands:\n");
  term_write("  help, clear/cls, echo, uptime, reboot\n");
  term_write("  ls, cat, cd, pwd\n");
  term_write("  mkdir, touch, write, append, rm\n");
  term_write("  mem, mount, ps, spawn, count\n");
  term_write("  exec <path>    (runs ELF in Ring3 via syscalls)\n");
  term_write("Tip: touch=file, mkdir=folder\n");
}

static void cmd_reboot(void) {
  __asm__ volatile("cli");
  outb(0x64, 0xFE);
  for (;;) __asm__ volatile("hlt");
}

static void cmd_uptime(void) {
  uint64_t ms = timer_uptime_ms();
  term_write("Uptime: ");
  term_write_u64(ms / 1000ULL);
  term_putc('.');
  term_write_3digits((uint32_t)(ms % 1000ULL));
  term_write("s  (ticks=");
  term_write_u64(timer_ticks());
  term_write(")\n");
}

static void cmd_mem(void) {
  term_write("Memory:\n  usable=");
  term_write_u32(pmm_total_kb());
  term_write("KB  free=");
  term_write_u32(pmm_free_kb());
  term_write("KB\n  heap used=");
  term_write_u32(kheap_used_bytes()/1024u);
  term_write("KB  free=");
  term_write_u32(kheap_free_bytes()/1024u);
  term_write("KB  total=");
  term_write_u32(kheap_total_bytes()/1024u);
  term_write("KB\n");
}

static void split_cmd_rest(const char* s, char cmd[16], char rest[LINE_MAX]) {
  s = skip_spaces(s);

  int ci = 0;
  while (*s && !is_space(*s) && ci < 15) cmd[ci++] = *s++;
  cmd[ci] = 0;

  s = skip_spaces(s);

  int ri = 0;
  while (*s && ri < LINE_MAX - 1) rest[ri++] = *s++;
  rest[ri] = 0;
  trim_right_inplace(rest);
}

static void run_command(const char* s) {
  char cmd[16];
  char rest[LINE_MAX];
  split_cmd_rest(s, cmd, rest);

  if (cmd[0] == 0) return;

  if (streq(cmd, "help")) { cmd_help(); return; }
  if (streq(cmd, "clear") || streq(cmd, "cls")) { term_clear(); prompt(); redraw_line(); return; }
  if (streq(cmd, "reboot")) { cmd_reboot(); return; }
  if (streq(cmd, "uptime")) { cmd_uptime(); return; }
  if (streq(cmd, "mem")) { cmd_mem(); return; }
  if (streq(cmd, "mount")) { vfs_list_mounts(); return; }
  if (streq(cmd, "ps")) { task_ps(); return; }

  if (streq(cmd, "spawn")) {
    int id = task_create("worker", bg_worker, 0);
    if (id < 0) term_write("spawn: failed\n");
    else term_write("spawn: ok\n");
    return;
  }

  if (streq(cmd, "count")) {
    term_write("counter=");
    term_write_u32(g_bg_counter);
    term_putc('\n');
    return;
  }

  if (streq(cmd, "exec")) {
    char path[256];
    parse_token(rest, path, (int)sizeof(path));
    if (!path[0]) { term_write("exec: missing path\n"); return; }

    char drv;
    const char* sub = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("exec: drive not mounted\n"); return; }
    if (!sub || !sub[0]) { term_write("exec: bad path\n"); return; }

    uint32_t base = (drv == cwd_drive) ? cwd_cluster : vfs_root_cluster(drv);

    uint8_t* img = 0;
    uint32_t sz = 0;
    if (fat32_read_file(base, sub, &img, &sz) != 0) {
      term_write("exec: read failed\n");
      return;
    }

    uint32_t entry = 0;
    if (elf_load(img, sz, &entry) != 0) {
      term_write("exec: bad elf\n");
      return;
    }

    int pid = task_create_user("app", entry);
    if (pid < 0) {
      term_write("exec: task create failed\n");
      return;
    }

    term_write("exec: started pid=");
    term_write_u32((uint32_t)pid);
    term_putc('\n');
    return;
  }

  if (streq(cmd, "echo")) {
    if (rest[0]) term_write(rest);
    term_putc('\n');
    return;
  }

  if (streq(cmd, "pwd")) {
    term_putc(cwd_drive);
    term_write(":");
    term_write(cwd_path);
    term_putc('\n');
    return;
  }

  if (streq(cmd, "ls")) {
    if (!rest[0]) { fat32_ls(cwd_cluster, "."); return; }
    char path[256];
    parse_token(rest, path, (int)sizeof(path));
    char drv;
    const char* p = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("ls: drive not mounted\n"); return; }
    if (!p || !p[0]) p = "/";
    uint32_t base = (drv == cwd_drive) ? cwd_cluster : vfs_root_cluster(drv);
    fat32_ls(base, p);
    return;
  }

  if (streq(cmd, "cat")) {
    char path[256];
    parse_token(rest, path, (int)sizeof(path));
    if (!path[0]) { term_write("cat: missing path\n"); return; }
    char drv;
    const char* p = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("cat: drive not mounted\n"); return; }
    if (!p || !p[0]) { term_write("cat: missing path\n"); return; }
    uint32_t base = (drv == cwd_drive) ? cwd_cluster : vfs_root_cluster(drv);
    fat32_cat(base, p);
    return;
  }

  if (streq(cmd, "cd")) {
    char path[256];
    if (!rest[0]) {
      cwd_drive = 'C';
      cwd_cluster = vfs_root_cluster(cwd_drive);
      cwd_path[0] = '/'; cwd_path[1] = 0;
      return;
    }

    parse_token(rest, path, (int)sizeof(path));
    if (!path[0]) { term_write("cd: missing path\n"); return; }

    int has_drive = (path[1] == ':');

    char drv;
    const char* p = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("cd: drive not mounted\n"); return; }
    if (!p || !p[0]) p = "/";

    uint32_t base = has_drive ? vfs_root_cluster(drv) : cwd_cluster;

    uint32_t newclus = 0;
    if (fat32_resolve_dir(base, p, &newclus) != 0) {
      term_write("cd: not found (use mkdir for folders)\n");
      return;
    }

    cwd_drive = drv;
    cwd_cluster = newclus;

    char newpath[PATH_MAX];
    const char* base_disp = (has_drive || p[0] == '/' || p[0] == '\\') ? "/" : cwd_path;
    path_normalize(base_disp, p, newpath);

    int i = 0;
    while (newpath[i] && i < PATH_MAX - 1) { cwd_path[i] = newpath[i]; i++; }
    cwd_path[i] = 0;
    return;
  }

  if (streq(cmd, "mkdir")) {
    char path[256];
    parse_token(rest, path, (int)sizeof(path));
    if (!path[0]) { term_write("mkdir: missing path\n"); return; }
    char drv;
    const char* p = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("mkdir: drive not mounted\n"); return; }
    if (!p || !p[0]) { term_write("mkdir: bad path\n"); return; }
    uint32_t base = (drv == cwd_drive) ? cwd_cluster : vfs_root_cluster(drv);
    fat32_mkdir(base, p);
    return;
  }

  if (streq(cmd, "touch")) {
    char path[256];
    parse_token(rest, path, (int)sizeof(path));
    if (!path[0]) { term_write("touch: missing path\n"); return; }
    char drv;
    const char* p = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("touch: drive not mounted\n"); return; }
    if (!p || !p[0]) { term_write("touch: bad path\n"); return; }
    uint32_t base = (drv == cwd_drive) ? cwd_cluster : vfs_root_cluster(drv);
    fat32_touch(base, p);
    return;
  }

  if (streq(cmd, "write") || streq(cmd, "append")) {
    char path[256];
    const char* p = parse_token(rest, path, (int)sizeof(path));
    p = skip_spaces(p);

    if (!path[0]) { term_write("write: missing path\n"); return; }
    if (!*p) { term_write("write: missing text\n"); return; }

    char drv;
    const char* sub = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("write: drive not mounted\n"); return; }
    if (!sub || !sub[0]) { term_write("write: bad path\n"); return; }

    uint32_t base = (drv == cwd_drive) ? cwd_cluster : vfs_root_cluster(drv);
    uint32_t n = (uint32_t)str_len(p);
    int app = streq(cmd, "append") ? 1 : 0;
    fat32_write(base, sub, (const uint8_t*)p, n, app);
    return;
  }

  if (streq(cmd, "rm")) {
    char path[256];
    parse_token(rest, path, (int)sizeof(path));
    if (!path[0]) { term_write("rm: missing path\n"); return; }
    char drv;
    const char* sub = vfs_strip_drive(path, cwd_drive, &drv);
    if (!vfs_is_mounted(drv)) { term_write("rm: drive not mounted\n"); return; }
    if (!sub || !sub[0]) { term_write("rm: bad path\n"); return; }
    uint32_t base = (drv == cwd_drive) ? cwd_cluster : vfs_root_cluster(drv);
    fat32_rm(base, sub);
    return;
  }

  term_write("Unknown command. Type 'help'.\n");
}

static void execute_line(void) {
  term_putc('\n');
  line[len] = 0;
  hist_push(line);
  run_command(line);

  len = 0;
  cur = 0;
  line[0] = 0;

  prompt();
  redraw_line();
}

static void autocomplete(void) {
  if (cur != len) return;
  for (size_t i = 0; i < len; i++) if (line[i] == ' ') return;

  int matches[32];
  int m = 0;

  for (int i = 0; i < commands_n; i++) {
    const char* c = commands[i];
    size_t plen = len;

    int ok = 1;
    for (size_t k = 0; k < plen; k++) {
      if (c[k] == 0 || c[k] != line[k]) { ok = 0; break; }
    }
    if (ok && m < (int)(sizeof(matches) / sizeof(matches[0]))) matches[m++] = i;
  }

  if (m == 0) return;

  if (m == 1) {
    const char* best = commands[matches[0]];
    set_line_from(best);
    return;
  }

  term_write("\n");
  for (int i = 0; i < m; i++) { term_write(commands[matches[i]]); term_write("  "); }
  prompt();
  redraw_line();
}

void shell_init(void) {
  cwd_drive = 'C';
  cwd_cluster = vfs_root_cluster(cwd_drive);
  if (cwd_cluster == 0) cwd_cluster = fat32_root_cluster();
  cwd_path[0] = '/'; cwd_path[1] = 0;

  term_write("Terminal ready.\n");
  prompt();
  redraw_line();
}

void shell_on_char(char c) {
  if (c == '\n') { execute_line(); return; }

  if ((unsigned char)c < 32 || (unsigned char)c > 126) return;
  if (len >= LINE_MAX - 1) return;

  int max_visible = 79 - prompt_x;
  if ((int)len >= max_visible) return;

  for (size_t i = len; i > cur; i--) line[i] = line[i - 1];
  line[cur] = c;
  len++;
  cur++;
  line[len] = 0;

  redraw_line();
}

void shell_on_key(key_t k) {
  switch (k) {
    case KEY_LEFT:
      if (cur > 0) { cur--; term_set_cursor(prompt_x + (int)cur, prompt_y); }
      break;
    case KEY_RIGHT:
      if (cur < len) { cur++; term_set_cursor(prompt_x + (int)cur, prompt_y); }
      break;
    case KEY_DELETE:
      if (cur < len) {
        for (size_t i = cur; i + 1 <= len; i++) line[i] = line[i + 1];
        len--;
        redraw_line();
      }
      break;
    case KEY_UP:
      if (hist_count == 0) break;
      if (hist_nav < hist_count - 1) hist_nav++;
      set_line_from(hist_get(hist_nav));
      break;
    case KEY_DOWN:
      if (hist_count == 0) break;
      if (hist_nav > 0) { hist_nav--; set_line_from(hist_get(hist_nav)); }
      else if (hist_nav == 0) { hist_nav = -1; set_line_from(""); }
      break;
    case KEY_TAB:
      autocomplete();
      break;
    default:
      break;
  }
}