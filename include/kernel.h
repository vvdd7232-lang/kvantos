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
#define KV_VERSION  "0.1.0 \"Photon\""
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

static inline u32 irq_save(void) {
    u32 fl; __asm__ volatile("pushfl; popl %0; cli":"=r"(fl)::"memory"); return fl;
}
static inline void irq_restore(u32 fl) {
    __asm__ volatile("pushl %0; popfl"::"r"(fl):"memory","cc");
}

/* ---------- register frame on interrupt ---------- */
typedef struct {
    u32 ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
} registers_t;

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
void kvsnprintf_v(char *buf, size_t size, const char *fmt, va_list ap);
void kvprintf(void (*emit)(char, void *), void *ctx, const char *fmt, va_list ap);

/* ---------- GDT / IDT / interrupts ---------- */
void gdt_init(void);
void idt_init(void);
void isr_install_handler(u8 n, isr_t h);
void irq_install_handler(u8 irq, isr_t h);
void pic_remap(void);
void set_kernel_stack(u32 esp0);

/* ---------- timer, keyboard, RTC ---------- */
void  timer_init(u32 hz);
u64   timer_ticks(void);
u32   timer_hz(void);
u32   timer_seconds(void);
void  sleep_ms(u32 ms);

void  keyboard_init(void);
int   kbd_getchar_nb(void);      /* -1 when no character is available */
char  kbd_getchar(void);         /* blocking read */
void  kbd_set_leds(u8 mask);     /* bits: 1 Scroll, 2 Num, 4 Caps */
void  kbd_poll(void);            /* fallback when IRQ1 never arrives */

#define KEY_UP     0x81
#define KEY_DOWN   0x82
#define KEY_LEFT   0x83
#define KEY_RIGHT  0x84

typedef struct { u8 sec, min, hour, day, month; u16 year; } rtc_time_t;
void rtc_read(rtc_time_t *t);

/* ---------- physical memory ---------- */
void   pmm_init(u32 mem_upper_kb, u32 mmap_addr, u32 mmap_len);
u32    pmm_alloc_frame(void);
void   pmm_free_frame(u32 addr);
void   pmm_reserve_range(u32 base, u32 size);
u32    pmm_total_frames(void);
u32    pmm_used_frames(void);
u32    pmm_total_bytes(void);

/* ---------- paging ---------- */
void   paging_init(void);
int    paging_map(u32 virt, u32 phys, int rw);
int    paging_map_range(u32 base, u32 size, int rw);
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
    u32          esp;
    u32          id;
    char         name[16];
    int          state;
    u64          wake_tick;
    u32          stack_base;
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

/* ---------- ramfs ---------- */
#define RAMFS_MAX_FILES 32
typedef struct { char name[24]; char *data; u32 size; int used; } rfile_t;
void    ramfs_init(void);
rfile_t *ramfs_find(const char *name);
int     ramfs_create(const char *name, const char *data, u32 size);
int     ramfs_delete(const char *name);
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
void fb_pixel(u32 x, u32 y, u32 c);
void fb_fill(i32 x, i32 y, i32 w, i32 h, u32 c);
void fb_rect(i32 x, i32 y, i32 w, i32 h, u32 c);
void fb_round_fill(i32 x, i32 y, i32 w, i32 h, u32 c);
void fb_gradient_v(i32 x, i32 y, i32 w, i32 h, u8 r1, u8 g1, u8 b1, u8 r2, u8 g2, u8 b2);
void fb_glyph(i32 x, i32 y, u8 ch, u32 fg, u32 bg);
void fb_text(i32 x, i32 y, const char *s, u32 fg, u32 bg);
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

/* ---------- miscellaneous ---------- */
void   shell_run(void);
void   panic(const char *msg, registers_t *r);
void   cpu_vendor(char *buf13);
void   cpu_brand(char *buf49);
void   kv_reboot(void);
void   kv_poweroff(void);
void   beep(u32 freq, u32 ms);
void   logo_print(void);

extern u32 kernel_start, kernel_end;
extern void gdt_flush(u32);
extern void tss_flush(void);
extern void idt_flush(u32);
extern void paging_enable(u32 pd_phys);
extern u32  read_cr2(void);
extern void context_switch(u32 *old_esp, u32 new_esp);

#endif
