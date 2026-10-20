/* ============================================================
 *  KvantOS - common kernel header
 * ============================================================ */
#ifndef _KVANT_KERNEL_H
#define _KVANT_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include "i18n.h"

#define KV_NAME     "KvantOS"
#define KV_VERSION  "3.2.0"
#define KV_ARCH     "i386 (32-bit protected mode)"
#define KV_BUILD    __DATE__ " " __TIME__

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;

/* ---------- I/O ports ---------- */
static inline void outb(u16 port, u8 val)  { __asm__ volatile("outb %0, %1"::"a"(val),"Nd"(port)); }
static inline u8   inb(u16 port)           { u8 r; __asm__ volatile("inb %1, %0":"=a"(r):"Nd"(port)); return r; }
static inline void outw(u16 port, u16 val) { __asm__ volatile("outw %0, %1"::"a"(val),"Nd"(port)); }
static inline u16  inw(u16 port)           { u16 r; __asm__ volatile("inw %1, %0":"=a"(r):"Nd"(port)); return r; }
static inline void outl(u16 port, u32 val) { __asm__ volatile("outl %0, %1"::"a"(val),"Nd"(port)); }
static inline u32  inl(u16 port)           { u32 r; __asm__ volatile("inl %1, %0":"=a"(r):"Nd"(port)); return r; }
static inline void io_wait(void)           { outb(0x80, 0); }
static inline void cli(void)               { __asm__ volatile("cli"); }
static inline void sti(void)               { __asm__ volatile("sti"); }
static inline void hlt(void)               { __asm__ volatile("hlt"); }

/* A machine word wide enough to hold a pointer: u32 on the legacy
   32-bit build, u64 on the 64-bit (KvantOS 3.0) build. Physical
   addresses stay u32 everywhere - the system lives under 4 GiB. */
typedef unsigned long kv_addr_t;
typedef unsigned long kv_flags_t;   /* whatever irq_save()/irq_restore() carry */

#ifdef __x86_64__
static inline u64 irq_save(void) {
    u64 fl; __asm__ volatile("pushfq; popq %0; cli":"=r"(fl)::"memory"); return fl;
}
static inline void irq_restore(u64 fl) {
    __asm__ volatile("pushq %0; popfq"::"r"(fl):"memory","cc");
}
#else
static inline u32 irq_save(void) {
    u32 fl; __asm__ volatile("pushfl; popl %0; cli":"=r"(fl)::"memory"); return fl;
}
static inline void irq_restore(u32 fl) {
    __asm__ volatile("pushl %0; popfl"::"r"(fl):"memory","cc");
}
#endif

/* ---------- register frame on interrupt ---------- */
#ifdef __x86_64__
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_no, err_code;
    u64 rip, cs, rflags, rsp, ss;
} registers_t;
#else
typedef struct {
    u32 ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
} registers_t;
#endif

typedef void (*isr_t)(registers_t *);

/* ---------- strings / memory ---------- */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
int    atoi(const char *s);
int    str_isnum(const char *s);
void   to_upper(char *s);

/* ---------- VGA text mode ---------- */
#define VGA_BLACK 0
#define VGA_BLUE 1
#define VGA_GREEN 2
#define VGA_CYAN 3
#define VGA_RED 4
#define VGA_MAGENTA 5
#define VGA_BROWN 6
#define VGA_LGREY 7
#define VGA_DGREY 8
#define VGA_LBLUE 9
#define VGA_LGREEN 10
#define VGA_LCYAN 11
#define VGA_LRED 12
#define VGA_LMAGENTA 13
#define VGA_YELLOW 14
#define VGA_WHITE 15
#define VGA_COLOR(fg, bg) ((u8)((bg) << 4 | (fg)))

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_set_color(u8 color);
u8   vga_get_color(void);
void vga_status(const char *left, const char *right, u8 color);
void vga_cursor_update(void);
void vga_panic_screen(void);
void vga_text_mode_restore(void);
u32  vga_cols(void);
u32  vga_rows(void);
int  vga_is_gfx(void);

/* ---------- serial port ---------- */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

/* ---------- formatted output ---------- */
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
void ksnprintf(char *buf, size_t size, const char *fmt, ...);
int  ksnprintf_ret(char *buf, size_t size, const char *fmt, ...);
void kvsnprintf_v(char *buf, size_t size, const char *fmt, va_list ap);
void kvprintf(void (*emit)(char, void *), void *ctx, const char *fmt, va_list ap);

/* ---------- GDT / IDT / interrupts ---------- */
void gdt_init(void);
void idt_init(void);
void isr_install_handler(u8 n, isr_t h);
void irq_install_handler(u8 irq, isr_t h);
void pic_remap(void);
void set_kernel_stack(kv_addr_t esp0);

/* ---------- timer, keyboard, RTC ---------- */
void  timer_init(u32 hz);
u64   timer_ticks(void);
u32   timer_hz(void);
u32   timer_seconds(void);
void  sleep_ms(u32 ms);

void  keyboard_init(void);
#include "keymap.h"
int   kbd_getevent_nb(kbd_event_t *event);
int   kbd_layout(void);
void  kbd_layout_set(int russian);
int   kbd_getchar_nb(void);      /* -1 when no character is available */
char  kbd_getchar(void);         /* blocking read */
void  kbd_set_leds(u8 mask);     /* bits: 1 Scroll, 2 Num, 4 Caps */
u8    kbd_get_leds(void);        /* the mask currently lit */
int   kbd_shift(void);
int   kbd_mousekeys(void);
void  kbd_mousekeys_set(int on);
void  kbd_poll(void);            /* fallback when IRQ1 never arrives */



typedef struct { u8 sec, min, hour, day, month; u16 year; } rtc_time_t;
void rtc_read(rtc_time_t *t);
/* Last range-checked RTC read. No CMOS I/O; -1 if absent/invalidated. */
int rtc_cached_clock(u32 hms[3]);
int  rtc_write_time(int h, int m, int s);   /* set the CMOS clock      */
int  rtc_write_date(int y, int mo, int d);  /* set the CMOS date       */

/* ---------- persistent settings (KvantOS 2.0) ----------
   Kept as a plain text file on KvFS ("settings.cfg") so that the
   choices made in one session survive the reboot. Without a mounted
   KvFS the functions quietly do nothing. */
void settings_load(void);             /* call once the disk is mounted */
void settings_save(void);             /* call after a setting changes  */

/* ---------- physical memory ---------- */
void   pmm_init(u32 mem_upper_kb, u32 mmap_addr, u32 mmap_len);
u32    pmm_alloc_frame(void);
void   pmm_free_frame(u32 addr);
void   pmm_reserve_range(u32 base, u32 size);
u32    pmm_total_frames(void);
u32    pmm_used_frames(void);
u32    pmm_total_bytes(void);

/* ---------- paging ---------- */
kv_addr_t get_kernel_stack(void);
int ku_mode_trap(registers_t *r);
int ku_mode_tick(registers_t *r);
void ku_selftest(void);
void   paging_init(void);
int    paging_map(u32 virt, u32 phys, int rw);
int    paging_map_range(u32 base, u32 size, int rw);
int    paging_map_mmio(u32 base, u32 size);
u32    paging_phys(u32 virt);

/* ---------- heap ---------- */
void   heap_init(u32 start, u32 size);
void  *kmalloc(size_t size);
void  *kcalloc(size_t n, size_t size);
void   kfree(void *p);
void   heap_stats(u32 *total, u32 *used, u32 *blocks);
char  *kstrdup(const char *s);

/* ---------- task scheduler ---------- */
#define TASK_READY    0
#define TASK_SLEEPING 1
#define TASK_DEAD     2

typedef struct task {
    kv_addr_t    esp;
    u32          id;
    char         name[16];
    int          state;
    u64          wake_tick;
    kv_addr_t    stack_base;
    u32          switches;
    struct task *next;
} task_t;

void    sched_init(void);
task_t *task_create(const char *name, void (*entry)(void));
void    schedule(void);
void    task_yield(void);
void    task_sleep(u32 ms);
void    task_exit(void);
task_t *task_current(void);
task_t *task_list(void);
u32     task_count(void);
int     task_kill(u32 id);       /* 0 ok, -1 no such task, -2 current */

/* ---------- ramfs ---------- */
#define RAMFS_MAX_FILES 32
typedef struct { char name[24]; char *data; u32 size; int used; } rfile_t;
void    ramfs_init(void);
rfile_t *ramfs_find(const char *name);
int     ramfs_create(const char *name, const char *data, u32 size);
int     ramfs_write(const char *name, const char *data, u32 size);
int     ramfs_delete(const char *name);
int     ramfs_rename(const char *from,const char *to); /* no-replace, no allocation */
rfile_t *ramfs_table(void);

/* ---------- ATA disk (PIO, LBA28) ---------- */
void        ata_init(void);
int         ata_count(void);
int         ata_boot_drive(void);
int         ata_present(void);
const char *ata_model(int i);
u32         ata_sectors(int i);
u32         ata_size_mb(int i);
int         ata_read(int idx, u32 lba, u8 count, void *buf);
int         ata_write(int idx, u32 lba, u8 count, const void *buf);

/* ---------- on-disk filesystem (KvFS) ---------- */
int         kvfs_mount(void);
int         kvfs_format(void);
int         kvfs_mounted(void);
int         kvfs_write(const char *name, const void *data, u32 size, int is_exec);
int         kvfs_read(const char *name, void *buf, u32 max);
int         kvfs_delete(const char *name);
int         kvfs_rename(const char *from,const char *to); /* vfs_rename status, NOT kvfs_error codes */
int         kvfs_exists(const char *name);
u32         kvfs_size(const char *name);
int         kvfs_list(int index, char *name, u32 *size, int *is_exec);
int         kvfs_file_count(void);
void        kvfs_stats(u32 *total_mb, u32 *used_kb, u32 *files);
const char *kvfs_error(int code);

/* ---------- installing the system onto a disk ---------- */
int         setup_available(void);
int         setup_install(int keep_files);
int         setup_progress(void);
int         setup_busy(void);
const char *setup_stage_text(void);
const char *setup_last_result(void);

/* ---------- .kapp applications ---------- */
int         kapp_load(const char *filename);
void        kapp_unload(void);
int         kapp_loaded(void);
int         kapp_isolated(void);
const char *kapp_name(void);
const char *kapp_filename(void);
const char *kapp_last_error(void);
const char *kapp_status(void);
i32         kapp_pref_w(void);
i32         kapp_pref_h(void);
void        kapp_draw(i32 x, i32 y, i32 w, i32 h);
void        kapp_tick(i32 x, i32 y, i32 w, i32 h);
void        kapp_key(int key, i32 x, i32 y, i32 w, i32 h);
void        kapp_click(i32 mx, i32 my, int button, i32 x, i32 y, i32 w, i32 h);
void        kapp_opened(i32 x, i32 y, i32 w, i32 h);
int         kapp_in_app(void);
void        kapp_note_fault(const char *why);
void        kapp_recover(const char *why);
void        gui_log(const char *s);

/* ---------- multiboot ---------- */
typedef struct {
    u32 flags;
    u32 mem_lower, mem_upper;
    u32 boot_device;
    u32 cmdline;
    u32 mods_count, mods_addr;
    u32 syms[4];
    u32 mmap_length, mmap_addr;
    u32 drives_length, drives_addr;
    u32 config_table;
    u32 boot_loader_name;
    u32 apm_table;
    u32 vbe_control_info, vbe_mode_info;
    u16 vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    u32 framebuffer_addr_low, framebuffer_addr_high;
    u32 framebuffer_pitch;
    u32 framebuffer_width, framebuffer_height;
    u8  framebuffer_bpp;
    u8  framebuffer_type;
    u8  fb_red_position, fb_red_mask_size;
    u8  fb_green_position, fb_green_mask_size;
    u8  fb_blue_position, fb_blue_mask_size;
} __attribute__((packed)) multiboot_info_t;

/* ---------- text transcoding ---------- */
u8   cp866_from_unicode(u32 cp);
u32  utf8_next(const char **s);
u32  utf8_len(const char *s);
u32  utf8_to_cp866(const char *s, u8 *out, u32 max);

/* ---------- linear framebuffer ---------- */
int  fb_init(const multiboot_info_t *mbi);
int  fb_map(void);

/* MTRR: write-combining for video memory (faster frame output) */
int  mtrr_set_wc(u32 base, u32 size);
int  mtrr_available(void);
int  mtrr_slot(void);
int  fb_remap(u32 phys, u32 w, u32 h, u32 pitch, u8 bpp);
void fb_console_resync(void);
int  fb_active(void);
u32  fb_width(void);
u32  fb_height(void);
u32  fb_pitch_get(void);
u8   fb_bpp_get(void);
u32  fb_base(void);
u32  fb_bytes(void);
u32  fb_rgb(u8 r, u8 g, u8 b);
void fb_set_target(void *p);
void *fb_get_hw(void);
void *fb_get_target(void);
typedef struct { i32 x, y, w, h; } fb_clip_rect_t;
void fb_set_clip_rects(const fb_clip_rect_t *rects, u32 count);
void fb_clear_clip(void);
void fb_copy_rect(void *dst, const void *src, i32 x, i32 y, i32 w, i32 h);
void fb_present_rect(const void *back, i32 x, i32 y, i32 w, i32 h);
void fb_pixel(u32 x, u32 y, u32 c);
void fb_fill(i32 x, i32 y, i32 w, i32 h, u32 c);
void fb_rect(i32 x, i32 y, i32 w, i32 h, u32 c);
void fb_round_fill(i32 x, i32 y, i32 w, i32 h, u32 c);
void fb_gradient_v(i32 x, i32 y, i32 w, i32 h, u8 r1, u8 g1, u8 b1, u8 r2, u8 g2, u8 b2);
void fb_glyph(i32 x, i32 y, u8 ch, u32 fg, u32 bg);
void fb_text(i32 x, i32 y, const char *s, u32 fg, u32 bg);
void fb_text_bold(i32 x, i32 y, const char *s, u32 fg, u32 bg);
void fb_text_scaled(i32 x, i32 y, const char *s, i32 scale, u32 fg, u32 bg);
void fb_set_aa(int on);
int  fb_aa_on(void);
void fb_blend_pixel(i32 x, i32 y, u32 c, u32 alpha);
void fb_line_aa(i32 x0, i32 y0, i32 x1, i32 y1, u32 c);
void fb_disc_aa(i32 cx, i32 cy, i32 r, u32 c);
void fb_round_fill_aa(i32 x, i32 y, i32 w, i32 h, i32 r, u32 c);
void fb_shadow(i32 x, i32 y, i32 w, i32 h, i32 r);
void fb_gradient_v_round(i32 x, i32 y, i32 w, i32 h, i32 r,
                         u8 r1, u8 g1, u8 b1, u8 r2, u8 g2, u8 b2);
void fb_text_center(i32 x, i32 y, i32 w, const char *s, u32 fg, u32 bg);
void fb_clear(u32 c);
void fb_present(const void *back);
void fb_present_rows(const void *back, u32 y0, u32 y1);
void fb_scroll_up(u32 top, u32 bottom, u32 dy, u32 bg);

/* ---------- PS/2 mouse ---------- */
void mouse_init(i32 w, i32 h);
int  mouse_present(void);
i32  mouse_x(void);
i32  mouse_y(void);
u8   mouse_buttons(void);
u32  mouse_events(void);
void mouse_set_bounds(i32 w, i32 h);
void mouse_set_pos(i32 x, i32 y);
int  mouse_wheel_delta(void);
void mouse_move_rel(i32 dx, i32 dy);
void mouse_set_button(int btn, int down);

/* USB mass storage and shared ATA/USB/AHCI sector routing. */
void usb_rescan(void);
int usb_count(void);
u32 usb_sectors(int disk);
u32 usb_generation(int disk);
int usb_fat_authorize(int disk,u32 start,u32 count,u32 generation);
void usb_fat_revoke(int disk);
int usb_read(int disk, u32 lba, u8 count, void *buf);
int usb_write(int disk,u32 lba,u8 count,const void *buf);
int usb_sync(int disk);
int usb_sense(int disk,u8 *key,u8 *asc,u8 *ascq);
#if KV_STORAGE_LAB
int usb_test_authorize(int disk,u32 start,u32 count);
int usb_test_authorize_pair(int disk,u32 start,u32 count,u32 journal,u32 journal_count);
void fat_lab(const char *mode,int cut);
void storage_lab(const char *mode);
#endif
const char *usb_status(void);
int ata_sync(int disk);
int blk_sync(int disk);
int blk_count(void);
u32 blk_sectors(int disk);
int blk_read(int disk, u32 lba, u8 count, void *buf);
int blk_write(int disk, u32 lba, u8 count, const void *buf);
int blk_is_usb(int disk);

/* AHCI SATA: default RO; FAT WAL leases only, no installer/KvFS routing. */
void ahci_init(void);
int ahci_count(void);
u32 ahci_sectors(int disk);
const char *ahci_model(int disk);
const char *ahci_status(int disk);
int ahci_read(int disk,u32 lba,u8 count,void *buf);
#define BLK_USB_BASE 4
#define BLK_AHCI_BASE 12 /* eight stable USB IDs remain 4..11 */
int blk_is_ahci(int disk);
u32 ahci_generation(int disk);
void ahci_rescan(void);
int ahci_fat_authorize(int disk,u32 start,u32 count,u32 generation);
void ahci_fat_revoke(int disk);
int ahci_write(int disk,u32 lba,u8 count,const void *buf);
int ahci_sync(int disk);
u32 blk_generation(int disk);
int blk_fat_authorize(int disk,u32 start,u32 count,u32 generation);
void blk_fat_revoke(int disk);

/* ---------- PCI bus ---------- */
#define PCI_MAX_DEV 48
typedef struct {
    u8  bus, slot, func;
    u16 vendor, device;
    u8  class_code, subclass, prog_if, revision;
    u8  header_type, irq;
    u32 bar[6];
    u8  bar_is_io[6];
} pci_dev_t;

void        pci_init(void);
u32         pci_count(void);
pci_dev_t  *pci_get(u32 i);
pci_dev_t  *pci_gpu(void);
u32         pci_read32(u8 bus, u8 slot, u8 func, u8 off);
void        pci_write32(u8 bus, u8 slot, u8 func, u8 off, u32 val);
u16         pci_read16(u8 bus, u8 slot, u8 func, u8 off);
u8          pci_read8(u8 bus, u8 slot, u8 func, u8 off);
const char *pci_vendor_name(u16 vid);
const char *pci_class_name(u8 cls, u8 sub);
const char *pci_gpu_model(u16 vid, u16 did);
u32         pci_bar_mem(const pci_dev_t *d, int *which);
u32         pci_bar_size(pci_dev_t *d, int idx);
void        pci_enable_device(pci_dev_t *d);

/* ---------- video mode control ---------- */
enum { GPU_NONE = 0, GPU_FIXED, GPU_BGA, GPU_VMWARE };
enum {
    VBE_OK = 0,
    VBE_WARN_VIRTUAL   = 1,
    VBE_ERR_UNSUPPORTED = -1,
    VBE_ERR_BADPARAM    = -2,
    VBE_ERR_NOVRAM      = -3,
    VBE_ERR_REJECTED    = -4,
    VBE_ERR_MAP         = -5
};

typedef struct { u32 width, height, bpp, pitch, phys, hz; } vmode_t;

void        vbe_init(void);
int         vbe_native_bringup(void);
int         vbe_bga_present(void);
int         vbe_force_text(void);
int         vbe_backend(void);
const char *vbe_backend_name(void);
u16         vbe_bga_version(void);
int         vbe_can_modeset(void);
void        vbe_current(vmode_t *m);
u32         vbe_mode_count(void);
int         vbe_mode_get(u32 i, vmode_t *m);
int         vbe_mode_fits(u32 w, u32 h, u32 bpp);
u32         vbe_vram_bytes(void);
int         vbe_set_mode(u32 w, u32 h, u32 bpp);
const char *vbe_error_text(int code);
int         vbe_set_refresh(u32 hz);
u32         vbe_get_refresh(void);
u32         vbe_measure_hz(void);
u32         vbe_count_vsync(void);

/* ---------- video subsystem commands ---------- */
void cmd_lspci(int verbose);
void cmd_gpuinfo(void);
void cmd_vidmode_list(void);
void cmd_vidmode(int argc, char **argv);
void cmd_refresh(int argc, char **argv);

/* ---------- graphical shell ---------- */
int  gui_run(void);
int  gui_active(void);   /* 1 while the desktop owns the framebuffer */

/* ---------- random numbers ---------- */
void   kv_rand_seed(u32 seed);
u32    kv_rand(void);            /* xorshift32, seeded from the timer */
u32    kv_rand_max(u32 n);       /* a number in [0, n) */

/* ---------- shell utilities (utilcmds.c) ---------- */
void   cmd_uname(void);
void   cmd_hostname(void);
void   cmd_whoami(void);
void   cmd_sysinfo(void);
void   cmd_calc(int argc, char **argv);
void   cmd_cal(int argc, char **argv);
void   cmd_hex(const char *arg);
void   cmd_bin(const char *arg);
void   cmd_dec(const char *arg);
void   cmd_rand(int argc, char **argv);
void   cmd_seq(int argc, char **argv);
void   cmd_rev(int argc, char **argv);
void   cmd_wc(const char *name);
void   cmd_grep(int argc, char **argv);
void   cmd_hexdump(const char *name);
void   cmd_sum(const char *name);
void   cmd_touch(const char *name);
void   cmd_cp(int argc, char **argv);
void   cmd_mv(int argc, char **argv);
void   cmd_kill(int argc, char **argv);
void   cmd_matrix(void);
void   cmd_fortune(void);
void   cmd_melody(void);
void   cmd_leds(int argc, char **argv);
void   cmd_ascii(void);
void   cmd_countdown(int argc, char **argv);
void   cmd_color(int argc, char **argv);
/* KvantOS 2.0 additions */
void   cmd_sort(const char *name);
void   cmd_uniq(const char *name);
void   cmd_tac(const char *name);
void   cmd_basename(const char *path);
void   cmd_dirname(const char *path);
void   cmd_repeat(int argc, char **argv);

/* ---------- miscellaneous ---------- */
void   shell_run(void);
void   shell_set_fg(u8 color);
void   panic(const char *msg, registers_t *r);
void   cpu_vendor(char *buf13);
void   cpu_brand(char *buf49);
void   kv_reboot(void);
void   kv_poweroff(void);
void   beep(u32 freq, u32 ms);
void   logo_print(void);

extern u32 kernel_start, kernel_end;
#ifdef __x86_64__
extern void gdt_flush(kv_addr_t);
extern void tss_flush(u16 sel);
extern void idt_flush(kv_addr_t);
extern char kernel_stack_top[];
#else
extern void gdt_flush(u32);
extern void tss_flush(void);
extern void idt_flush(u32);
#endif
extern void paging_enable(kv_addr_t pd_phys);
extern kv_addr_t read_cr2(void);
extern void context_switch(kv_addr_t *old_esp, kv_addr_t new_esp);

#endif
