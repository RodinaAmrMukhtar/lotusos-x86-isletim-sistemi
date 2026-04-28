#include "terminal.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "shell.h"
#include "timer.h"
#include "mbr.h"
#include "fat32.h"

#include "bootinfo.h"
#include "multiboot2.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "vfs.h"
#include "task.h"
#include "gdt.h"
#include "isr.h"

boot_info_t* bootinfo_from_multiboot2(uint32_t mb2_info_phys);
void kmain(uint32_t bootinfo_ptr);

#include "events.h"
#include "mouse.h"
#include "gfx.h"
#include "wm.h"
#include "splash.h"

#include <stdint.h>

/* The bundled disk image (build/os.img) usually contains the FAT32 volume
   written at LBA 2048 (see Makefile FS_LBA). If a real MBR partition exists,
   we'll detect it and override this. We also support a "superfloppy" FAT32 at LBA 0. */
static uint32_t g_fs_lba = 2048;
extern uint32_t __kernel_end;

/* keep in sync with timer_init(...) below */
static const uint32_t K_TIMER_HZ = 600;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }
static uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1u); }

static void map_framebuffer_if_any(const boot_info_t* bi) {
  if (!bi || bi->magic != BOOTINFO_MAGIC) return;
  if (bi->fb_addr == 0 || bi->fb_pitch == 0 || bi->fb_height == 0) return;

  uint32_t fb_base = align_down(bi->fb_addr, 0x1000u);
  uint32_t fb_size = bi->fb_pitch * bi->fb_height;
  uint32_t fb_end  = align_up(bi->fb_addr + fb_size, 0x1000u);

  for (uint32_t p = fb_base; p < fb_end; p += 0x1000u) {
    paging_map(p, p, PAGE_PRESENT | PAGE_RW);
  }
}

static void rect_union(int ax,int ay,int aw,int ah, int bx,int by,int bw,int bh,
                       int* ox,int* oy,int* ow,int* oh) {
  int x0 = ax < bx ? ax : bx;
  int y0 = ay < by ? ay : by;
  int x1 = (ax + aw) > (bx + bw) ? (ax + aw) : (bx + bw);
  int y1 = (ay + ah) > (by + bh) ? (ay + ah) : (by + bh);
  if (ox) *ox = x0;
  if (oy) *oy = y0;
  if (ow) *ow = x1 - x0;
  if (oh) *oh = y1 - y0;
}

static void repaint_rect(int x, int y, int w, int h) {
  wm_paint_rect(x, y, w, h);
  gfx_present_rect(x, y, w, h);
  wm_draw_cursor();
}

static void repaint_full(void) {
  wm_render_full();
  gfx_present();
  wm_draw_cursor();
}

/* cursor overlay only (NO wm_paint_rect) */
static void repaint_cursor_only(int x, int y, int w, int h) {
  gfx_present_rect(x, y, w, h);
  wm_draw_cursor();
}


void kentry(uint32_t boot_magic, uint32_t boot_info_ptr) {
  if (boot_magic != MB2_BOOTLOADER_MAGIC) {
    /* If this trips, the kernel wasn't booted via Multiboot2. */
    for (;;) { __asm__ __volatile__("hlt"); }
  }

  boot_info_t* bi = bootinfo_from_multiboot2(boot_info_ptr);
  kmain((uint32_t)(uintptr_t)bi);
}

void kmain(uint32_t bootinfo_ptr) {
  gdt_init();
  boot_info_t* bi = (boot_info_t*)(uintptr_t)bootinfo_ptr;

  uint32_t kernel_start = 0x00100000u;
  uint32_t kernel_end   = (uint32_t)(uintptr_t)&__kernel_end;

  uint32_t heap_start = align_up(kernel_end, 0x1000u);
  uint32_t heap_end_want = heap_start + (32u * 1024u * 1024u);

  pmm_init(bi, kernel_start, kernel_end, heap_start, heap_end_want);

  uint32_t heap_end = heap_end_want;
  uint32_t max_phys = pmm_max_phys();
  if (heap_end > max_phys) heap_end = max_phys;
  if (heap_end < heap_start) heap_end = heap_start;

  uint32_t map_mb = (max_phys + (1024u * 1024u) - 1u) / (1024u * 1024u);
  if (map_mb < 64) map_mb = 64;
  if (map_mb > 512) map_mb = 512;
  paging_init_identity(map_mb);

  map_framebuffer_if_any(bi);

  term_init(bi);
  kheap_init(heap_start, heap_end);

  idt_init();
  isr_init();

  pic_remap(0x20, 0x28);
  pic_set_masks(0xFF, 0xFF);

  timer_init(K_TIMER_HZ);
  events_init();
  keyboard_init();

  vfs_init();
  {
    uint32_t lba = 0, secs = 0;
    if (mbr_find_first_fat32(&lba, &secs) == 0) {
      g_fs_lba = lba;
      (void)fat32_mount(g_fs_lba);
    } else {
      /* No valid MBR partition table.
         Try the common layout used by this project (FAT32 at LBA 2048),
         then fall back to superfloppy at LBA 0. */
      if (fat32_mount(2048) == 0) g_fs_lba = 2048;
      else if (fat32_mount(0) == 0) g_fs_lba = 0;
      else {
        term_write("FAT32 mount failed (tried LBA 2048 and 0)\n");
      }
    }

    vfs_mount_fat32('C', g_fs_lba);
  }

  task_init();
  shell_init();
  mouse_init(bi);

  pic_set_masks(0xF8, 0xEF);

  if (gfx_init(bi) != 0) {
    for (;;) __asm__ volatile("cli; hlt");
  }

  __asm__ volatile("sti");

  splash_show(bi, 200);

  { event_t tmp; while (events_pop(&tmp)) {} }

  wm_init(bi);
  repaint_full();

  int old_cx=0, old_cy=0, old_cw=0, old_ch=0;
  wm_get_cursor_rect(&old_cx, &old_cy, &old_cw, &old_ch);

  uint64_t last_paint_ms = timer_uptime_ms();

  /* pending scene repaint (done on EV_TICK) */
  int pending_full = 0;
  int pending_valid = 0;
  int px=0, py=0, pw=0, ph=0;

  for (;;) {
    /* Batch a handful of queued events to reduce redraw overhead. */
    event_t q[32];
    int qn = 0;

    events_wait(&q[qn++]);
    while (qn < (int)(sizeof(q) / sizeof(q[0])) && events_pop(&q[qn])) qn++;

    int saw_tick = 0;
    int saw_key  = 0;
    int saw_mouse = 0;

    /* Track cursor movement across the whole batch. */
    int batch_cx0 = old_cx, batch_cy0 = old_cy, batch_cw0 = old_cw, batch_ch0 = old_ch;
    int batch_cx1 = old_cx, batch_cy1 = old_cy, batch_cw1 = old_cw, batch_ch1 = old_ch;

    for (int ei = 0; ei < qn; ei++) {
      event_t ev = q[ei];

      int consumed = wm_handle_event(&ev);
      uint32_t rf = wm_take_redraw_flags();

      if (!consumed) {
        if (ev.type == EV_CHAR) { shell_on_char((char)ev.code); rf |= WM_RF_TERM; saw_key = 1; }
        else if (ev.type == EV_KEY) { shell_on_key((key_t)ev.code); rf |= WM_RF_TERM; saw_key = 1; }
      }

      if (ev.type == EV_TICK) {
        saw_tick = 1;

        /* Key repeat generation on each tick */
        event_t rep;
        while (keyboard_repeat_event(&rep)) {
          int c2 = wm_handle_event(&rep);
          uint32_t rf2 = wm_take_redraw_flags();
          rf |= rf2;
          saw_key = 1;

          if (!c2) {
            if (rep.type == EV_CHAR) { shell_on_char((char)rep.code); rf |= WM_RF_TERM; }
            else if (rep.type == EV_KEY) { shell_on_key((key_t)rep.code); rf |= WM_RF_TERM; }
          }
        }
      }

      if (ev.type == EV_MOUSE) saw_mouse = 1;

      /* Cursor union (for restoring old cursor pixels quickly). */
      int new_cx=0, new_cy=0, new_cw=0, new_ch=0;
      wm_get_cursor_rect(&new_cx, &new_cy, &new_cw, &new_ch);

      batch_cx1 = new_cx; batch_cy1 = new_cy; batch_cw1 = new_cw; batch_ch1 = new_ch;

      int ux, uy, uw, uh;
      rect_union(old_cx, old_cy, old_cw, old_ch, new_cx, new_cy, new_cw, new_ch, &ux, &uy, &uw, &uh);

      int rx = ux, ry = uy, rw = uw, rh = uh;

      if (rf & WM_RF_FULL) {
        pending_full = 1;
      } else if (rf & WM_RF_DIRTY) {
        int dx, dy, dw, dh;
        if (wm_take_dirty_rect(&dx, &dy, &dw, &dh)) {
          rect_union(rx,ry,rw,rh, dx,dy,dw,dh, &rx,&ry,&rw,&rh);
        }
      } else if (rf & (WM_RF_TERM | WM_RF_DEMO | WM_RF_NOTE | WM_RF_EXPL | WM_RF_TRASH | WM_RF_TASKBAR)) {
        if (rf & WM_RF_TERM)  { int x,y,w,h; wm_get_paint_rect(WM_WIN_TERM, &x,&y,&w,&h); if (w&&h) rect_union(rx,ry,rw,rh,x,y,w,h,&rx,&ry,&rw,&rh); }
        if (rf & WM_RF_DEMO)  { int x,y,w,h; wm_get_paint_rect(WM_WIN_DEMO, &x,&y,&w,&h); if (w&&h) rect_union(rx,ry,rw,rh,x,y,w,h,&rx,&ry,&rw,&rh); }
        if (rf & WM_RF_NOTE)  { int x,y,w,h; wm_get_paint_rect(WM_WIN_NOTE, &x,&y,&w,&h); if (w&&h) rect_union(rx,ry,rw,rh,x,y,w,h,&rx,&ry,&rw,&rh); }
        if (rf & WM_RF_EXPL)  { int x,y,w,h; wm_get_paint_rect(WM_WIN_EXPL, &x,&y,&w,&h); if (w&&h) rect_union(rx,ry,rw,rh,x,y,w,h,&rx,&ry,&rw,&rh); }
        if (rf & WM_RF_TRASH) { int x,y,w,h; wm_get_paint_rect(WM_WIN_TRASH, &x,&y,&w,&h); if (w&&h) rect_union(rx,ry,rw,rh,x,y,w,h,&rx,&ry,&rw,&rh); }
        if (rf & WM_RF_TASKBAR) { int x=0,y=gfx_h()-110,w=gfx_w(),h=110; rect_union(rx,ry,rw,rh,x,y,w,h,&rx,&ry,&rw,&rh); }
      }

      if (rf != WM_RF_NONE) {
        if (pending_valid) rect_union(px,py,pw,ph, rx,ry,rw,rh, &px,&py,&pw,&ph);
        else { px=rx; py=ry; pw=rw; ph=rh; pending_valid = 1; }
      }

      old_cx = new_cx; old_cy = new_cy; old_cw = new_cw; old_ch = new_ch;
    }

    /* ----- one paint decision for the entire batch ----- */
    uint64_t now_ms = timer_uptime_ms();
    int throttle_ok = (now_ms - last_paint_ms) >= 10;

    if ((pending_full || pending_valid)) {
      int paint_now = 0;

      /* make typing feel instant */
      if (saw_key) paint_now = 1;

      /* mouse movement can spam; keep it smooth but capped */
      if (!paint_now && saw_mouse && throttle_ok) paint_now = 1;

      /* animation pacing */
      if (!paint_now && saw_tick) paint_now = 1;

      if (paint_now) {
        if (pending_full) repaint_full();
        else if (pending_valid) repaint_rect(px, py, pw, ph);
        pending_full = 0;
        pending_valid = 0;
        last_paint_ms = now_ms;
        continue;
      }
    }

    /* If only the cursor moved, restore + redraw cursor from the backbuffer. */
    if (saw_mouse) {
      int ux, uy, uw, uh;
      rect_union(batch_cx0, batch_cy0, batch_cw0, batch_ch0,
                 batch_cx1, batch_cy1, batch_cw1, batch_ch1,
                 &ux, &uy, &uw, &uh);
      repaint_cursor_only(ux, uy, uw, uh);
    }

    /* Otherwise, wait for the next event (pending paints stay pending). */
  }
}