/* ============================================================
 *  KvantOS - the .kapp application loader and syscall table
 *
 *  How it works:
 *    1. The file is read from disk (KvFS) into a buffer.
 *    2. The header is validated: signature, ABI version, sizes.
 *    3. The image is copied to the fixed address KAPP_LOAD_BASE and
 *       the bss tail is zeroed.
 *    4. kapp_main(&api) is called - the application returns its own
 *       description together with event handlers.
 *    5. From then on the shell calls on_draw/on_key/on_click itself.
 *
 *  An application is built for the absolute address KAPP_LOAD_BASE, so
 *  it needs no relocation - there are no relocation tables at all. The
 *  price of that simplicity: exactly one application can run at a time.
 *  For a system of this size that is an honest trade.
 *
 *  Applications run in the kernel ring. To stop a broken program from
 *  dragging the whole system down, every call into it is wrapped in a
 *  "safety net": the exception handler notes that the fault happened
 *  inside an application and unloads the culprit instead of panicking
 *  the kernel (see kapp_guard_* below).
 * ============================================================ */
#include "kernel.h"
#include "kvapp.h"

/* ---------- state of the loaded application ---------- */
static int        loaded = 0;
static kv_app_t  *app    = NULL;
static char       app_file[40];
static char       app_title[64];
static char       last_error[96];

/* The client area the application draws into. The shell sets it
   before every handler call: the application assumes the origin is
   the top left corner of its own window. */
static i32 cl_x, cl_y, cl_w, cl_h;

/* ============================================================
 *  The safety net
 *
 *  An application runs in the kernel ring, so a bug in it raises a CPU
 *  exception, which normally means a panic and a halted system. That
 *  behaviour is unacceptable: a broken game must not kill the whole OS.
 *
 *  Therefore, before every entry into an application we record a
 *  return point (registers esp, ebp, ebx, esi, edi and the resume
 *  address). When the exception handler sees the in_app flag it does
 *  not panic but jumps back to that point - just like longjmp in plain
 *  C. The application is unloaded and the shell keeps running.
 * ============================================================ */
typedef struct { u32 esp, ebp, ebx, esi, edi, eip; } kapp_jmp_t;

static kapp_jmp_t    guard_buf;
static volatile int  in_app = 0;
static volatile int  app_faulted = 0;
static char          fault_reason[64];

/* Saving the return point.
 *
 *  A subtlety that is easy to get burned by: the registers must be
 *  saved by the CALLING code itself, not by a separate function. Hide
 *  this in a function and it will record the esp of its own frame,
 *  return, the frame disappears - and the jump back lands in an
 *  overwritten stack. Hence a macro: it expands directly inside
 *  enter(), whose frame lives for as long as the application handler
 *  is running.
 */
#define GUARD_SET(b, res)                        \
    __asm__ volatile(                            \
        "movl %%esp, 0(%1)\n\t"                  \
        "movl %%ebp, 4(%1)\n\t"                  \
        "movl %%ebx, 8(%1)\n\t"                  \
        "movl %%esi, 12(%1)\n\t"                 \
        "movl %%edi, 16(%1)\n\t"                 \
        "movl $1f, 20(%1)\n\t"                   \
        "xorl %0, %0\n\t"                        \
        "jmp 2f\n"                               \
        "1:\n\t"                                 \
        "movl $1, %0\n"                          \
        "2:\n\t"                                 \
        : "=&r"(res)                             \
        : "r"(b)                                 \
        : "memory", "cc")

/* Jump back to the saved point. Never returns. */
static __attribute__((noreturn)) void guard_jump(kapp_jmp_t *b) {
    __asm__ volatile(
        "movl 0(%0), %%esp\n\t"
        "movl 4(%0), %%ebp\n\t"
        "movl 8(%0), %%ebx\n\t"
        "movl 12(%0), %%esi\n\t"
        "movl 16(%0), %%edi\n\t"
        "sti\n\t"
        "jmp *20(%0)\n"
        :: "r"(b) : "memory");
    __builtin_unreachable();
}

int kapp_in_app(void) { return in_app; }

void kapp_note_fault(const char *why) {
    app_faulted = 1;
    strncpy(fault_reason, why ? why : T("exception", "исключение"), sizeof(fault_reason));
    fault_reason[sizeof(fault_reason) - 1] = 0;
}

/* Called by the exception handler instead of panic() when the fault
   happened inside an application. Control never returns here. */
void kapp_recover(const char *why) {
    kapp_note_fault(why);
    in_app = 0;
    guard_jump(&guard_buf);
}

/* ============================================================
 *  Implementation of the system functions offered to applications
 * ============================================================ */

static kv_i32 api_width(void)  { return cl_w; }
static kv_i32 api_height(void) { return cl_h; }

static kv_u32 api_rgb(kv_u8 r, kv_u8 g, kv_u8 b) { return fb_rgb(r, g, b); }

/* A rectangle in window coordinates, clipped to the client area.
   This is exactly where an application loses the ability to draw over
   other windows and the taskbar: everything outside is cut away before
   fb_fill is called. */
static void clip_fill(kv_i32 x, kv_i32 y, kv_i32 w, kv_i32 h, kv_u32 c) {
    if (w <= 0 || h <= 0) return;
    i32 ax = cl_x + x, ay = cl_y + y;
    i32 bx = ax + w,   by = ay + h;
    if (ax < cl_x) ax = cl_x;
    if (ay < cl_y) ay = cl_y;
    if (bx > cl_x + cl_w) bx = cl_x + cl_w;
    if (by > cl_y + cl_h) by = cl_y + cl_h;
    if (bx <= ax || by <= ay) return;
    fb_fill(ax, ay, bx - ax, by - ay, c);
}

static void api_clear(kv_u32 c) { clip_fill(0, 0, cl_w, cl_h, c); }

static void api_pixel(kv_i32 x, kv_i32 y, kv_u32 c) {
    if (x < 0 || y < 0 || x >= cl_w || y >= cl_h) return;
    fb_pixel((u32)(cl_x + x), (u32)(cl_y + y), c);
}

static void api_fill(kv_i32 x, kv_i32 y, kv_i32 w, kv_i32 h, kv_u32 c) {
    clip_fill(x, y, w, h, c);
}

static void api_rect(kv_i32 x, kv_i32 y, kv_i32 w, kv_i32 h, kv_u32 c) {
    if (w <= 0 || h <= 0) return;
    clip_fill(x, y, w, 1, c);
    clip_fill(x, y + h - 1, w, 1, c);
    clip_fill(x, y, 1, h, c);
    clip_fill(x + w - 1, y, 1, h, c);
}

/* A Bresenham line: without it any chart turns into an ordeal of
   individual dots. */
static void api_line(kv_i32 x0, kv_i32 y0, kv_i32 x1, kv_i32 y1, kv_u32 c) {
    i32 dx = x1 - x0, dy = y1 - y0;
    i32 sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    i32 err = dx - dy;
    for (int guard = 0; guard < 8192; guard++) {
        api_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        i32 e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Text clipped at the right edge of the window: drawn character by
   character, stopping as soon as the next one no longer fits. */
static void api_text(kv_i32 x, kv_i32 y, const char *s, kv_u32 fg, kv_u32 bg) {
    if (!s) return;
    if (y + KV_CHAR_H <= 0 || y >= cl_h) return;
    while (*s) {
        if (x >= cl_w) break;
        u32 cp = utf8_next(&s);
        if (x >= 0 && y >= 0 && x + KV_CHAR_W <= cl_w && y + KV_CHAR_H <= cl_h)
            fb_glyph(cl_x + x, cl_y + y, cp866_from_unicode(cp), fg, bg);
        x += KV_CHAR_W;
    }
}

static kv_i32 api_text_width(const char *s) {
    return s ? (kv_i32)(utf8_len(s) * KV_CHAR_W) : 0;
}

/* The application writes the status line into a buffer, the shell draws it */
static char app_status[96];
static void api_status(const char *s) {
    strncpy(app_status, s ? s : "", sizeof(app_status));
    app_status[sizeof(app_status) - 1] = 0;
}
const char *kapp_status(void) { return app_status; }

static void *api_alloc(kv_u32 n)  { return kmalloc(n); }
static void  api_release(void *p) { kfree(p); }

static kv_u32 api_ticks(void)   { return (u32)timer_ticks(); }
static kv_u32 api_hz(void)      { return timer_hz(); }
static kv_u32 api_seconds(void) { return timer_seconds(); }

static void api_clock(kv_i32 *h, kv_i32 *m, kv_i32 *s) {
    rtc_time_t t;
    rtc_read(&t);
    if (h) *h = t.hour;
    if (m) *m = t.min;
    if (s) *s = t.sec;
}

static void api_beep(kv_u32 f, kv_u32 ms) {
    if (ms > 1000) ms = 1000;      /* an application must not hang the system with beeping */
    beep(f, ms);
}

static kv_i32 api_file_read(const char *name, void *buf, kv_u32 max) {
    return kvfs_read(name, buf, max);
}
static kv_i32 api_file_write(const char *name, const void *buf, kv_u32 size) {
    return kvfs_write(name, buf, size, 0);
}
static kv_i32 api_file_delete(const char *name) { return kvfs_delete(name); }
static kv_i32 api_file_list(kv_i32 i, char *name40, kv_u32 *size) {
    return kvfs_list(i, name40, size, NULL);
}
static kv_i32 api_file_exists(const char *name) { return kvfs_exists(name); }

static void  *api_memset(void *d, int c, kv_u32 n)        { return memset(d, c, n); }
static void  *api_memcpy(void *d, const void *s, kv_u32 n){ return memcpy(d, s, n); }
static kv_u32 api_strlen(const char *s)                   { return (u32)strlen(s); }
static kv_i32 api_strcmp(const char *a, const char *b)    { return strcmp(a, b); }
static void   api_strcpy(char *d, const char *s, kv_u32 max) { strncpy(d, s, max); }

static void api_format(char *buf, kv_u32 size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf_v(buf, size, fmt, ap);
    va_end(ap);
}

/* A linear congruential generator: good enough for applications,
   and there is no reason to drag anything serious into the kernel. */
static u32 rnd_state = 2463534242u;
static kv_u32 api_random(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state ^ (u32)timer_ticks();
}

static void api_log(const char *s) { gui_log(s); }

/* The system language for an application: 0 - English, 1 - Russian. */
static kv_u32 api_lang(void) { return (kv_u32)kv_lang_get(); }

static const kv_api_t api_table = {
    .api_version = KV_API_VERSION,
    .width = api_width,   .height = api_height,
    .rgb = api_rgb,       .clear = api_clear,
    .pixel = api_pixel,   .fill = api_fill,
    .rect = api_rect,     .line = api_line,
    .text = api_text,     .text_width = api_text_width,
    .status = api_status,
    .alloc = api_alloc,   .release = api_release,
    .ticks = api_ticks,   .hz = api_hz, .seconds = api_seconds,
    .clock = api_clock,   .beep = api_beep,
    .file_read = api_file_read,     .file_write = api_file_write,
    .file_delete = api_file_delete, .file_list = api_file_list,
    .file_exists = api_file_exists,
    .mem_set = api_memset, .mem_copy = api_memcpy,
    .str_len = api_strlen, .str_cmp = api_strcmp, .str_copy = api_strcpy,
    .format = api_format,
    .random = api_random,  .log = api_log,
    .lang = api_lang,
};

/* ============================================================
 *  Loading and unloading
 * ============================================================ */

const char *kapp_last_error(void) { return last_error; }
int         kapp_loaded(void)     { return loaded; }
const char *kapp_name(void)       { return loaded ? app_title : ""; }
const char *kapp_filename(void)   { return loaded ? app_file : ""; }

kv_i32 kapp_pref_w(void) { return (loaded && app && app->width  > 0) ? app->width  : 420; }
kv_i32 kapp_pref_h(void) { return (loaded && app && app->height > 0) ? app->height : 300; }

/* Header validation lives in its own function: clear diagnostics
   matter more than brevity - a vague "it does not start" is the most
   infuriating message of all. */
static int check_header(const kapp_header_t *h, u32 filesize) {
    if (h->magic[0] != KAPP_MAGIC0 || h->magic[1] != KAPP_MAGIC1 ||
        h->magic[2] != KAPP_MAGIC2 || h->magic[3] != KAPP_MAGIC3) {
        strncpy(last_error, T("not a KvantOS application (no KAPP signature)", "это не приложение KvantOS (нет подписи KAPP)"), sizeof(last_error));
        return -1;
    }
    if (h->version != KAPP_FORMAT_VERSION) {
        ksnprintf(last_error, sizeof(last_error),
                  T("file format version %u, the kernel speaks %u", "формат файла версии %u, ядро понимает %u"), h->version, KAPP_FORMAT_VERSION);
        return -1;
    }
    if (h->api_version != KV_API_VERSION) {
        ksnprintf(last_error, sizeof(last_error),
                  T("built against ABI %u, the system provides ABI %u", "приложение собрано под ABI %u, в системе ABI %u"), h->api_version, KV_API_VERSION);
        return -1;
    }
    if (h->load_base != KAPP_LOAD_BASE) {
        strncpy(last_error, T("bad load address in the header", "неверный адрес загрузки в заголовке"), sizeof(last_error));
        return -1;
    }
    if (h->code_size == 0 || h->code_size > filesize - h->header_size) {
        strncpy(last_error, T("code size in the header does not match the file", "размер кода в заголовке не совпадает с файлом"), sizeof(last_error));
        return -1;
    }
    if (h->code_size + h->bss_size > KAPP_MAX_SIZE) {
        strncpy(last_error, T("application larger than 2 MiB", "приложение больше 2 МиБ"), sizeof(last_error));
        return -1;
    }
    if (h->entry < KAPP_LOAD_BASE || h->entry >= KAPP_LOAD_BASE + h->code_size) {
        strncpy(last_error, T("entry point outside the application image", "точка входа вне образа приложения"), sizeof(last_error));
        return -1;
    }
    return 0;
}

void kapp_unload(void) {
    if (!loaded) return;
    if (app && app->on_close && !app_faulted) {
        int urc;
        GUARD_SET(&guard_buf, urc);
        if (urc == 0) {
            in_app = 1;
            app->on_close();
            in_app = 0;
        }
    }
    loaded = 0;
    app = NULL;
    app_file[0] = 0;
    app_status[0] = 0;
}

/* Loads an application from a KvFS file. 0 on success, otherwise see kapp_last_error() */
int kapp_load(const char *filename) {
    last_error[0] = 0;
    app_faulted = 0;

    if (loaded) kapp_unload();

    /* Look for the application on the disk first, then in ramfs.
       The latter allows running programs embedded in the boot image:
       on a machine with no formatted disk that is the only way to run
       anything at all. */
    u32 fsize = kvfs_mounted() ? kvfs_size(filename) : 0;
    rfile_t *rf = fsize ? NULL : ramfs_find(filename);

    if (!fsize && !rf) {
        ksnprintf(last_error, sizeof(last_error), T("file '%s' not found", "файл '%s' не найден"), filename);
        return -1;
    }
    if (!fsize) fsize = rf->size;

    if (fsize < sizeof(kapp_header_t)) {
        strncpy(last_error, T("file too small to be an application", "файл слишком мал для приложения"), sizeof(last_error));
        return -1;
    }

    u8 *tmp = (u8 *)kmalloc(fsize);
    if (!tmp) {
        strncpy(last_error, T("not enough memory to load it", "не хватает памяти для загрузки"), sizeof(last_error));
        return -1;
    }

    int got;
    if (rf) {
        memcpy(tmp, rf->data, fsize);
        got = (int)fsize;
    } else {
        got = kvfs_read(filename, tmp, fsize);
    }
    if (got < (int)sizeof(kapp_header_t)) {
        kfree(tmp);
        strncpy(last_error, T("file read error", "ошибка чтения файла"), sizeof(last_error));
        return -1;
    }

    kapp_header_t hdr;
    memcpy(&hdr, tmp, sizeof(hdr));
    if (check_header(&hdr, (u32)got) < 0) { kfree(tmp); return -1; }

    /* Unpack the image at the fixed address */
    u8 *dst = (u8 *)KAPP_LOAD_BASE;
    memcpy(dst, tmp + hdr.header_size, hdr.code_size);
    if (hdr.bss_size) memset(dst + hdr.code_size, 0, hdr.bss_size);
    kfree(tmp);

    /* Flush the instruction cache: we have just written code as data */
    __asm__ volatile("" ::: "memory");

    strncpy(app_file, filename, sizeof(app_file));
    app_file[sizeof(app_file) - 1] = 0;
    strncpy(app_title, hdr.name[0] ? hdr.name : filename, sizeof(app_title));
    app_title[sizeof(app_title) - 1] = 0;
    app_status[0] = 0;

    /* Call the entry point under the safety net */
    kapp_entry_t entry = (kapp_entry_t)hdr.entry;
    kv_app_t *result = NULL;
    int grc;
    GUARD_SET(&guard_buf, grc);
    if (grc == 0) {
        in_app = 1;
        result = entry(&api_table);
        in_app = 0;
    }

    if (app_faulted) {
        ksnprintf(last_error, sizeof(last_error), T("startup failure: %s", "сбой при запуске: %s"), fault_reason);
        return -1;
    }
    if (!result) {
        strncpy(last_error, T("kapp_main() returned null", "kapp_main() вернула ноль"), sizeof(last_error));
        return -1;
    }

    app = result;
    loaded = 1;
    if (app->title && app->title[0]) {
        strncpy(app_title, app->title, sizeof(app_title));
        app_title[sizeof(app_title) - 1] = 0;
    }
    return 0;
}

/* ============================================================
 *  Calling the handlers from the shell
 *
 *  Every entry into an application is framed by setting the window and
 *  the in_app flag. Should an exception occur inside, panic() sees the
 *  flag, reports it and unloads the application instead of halting the
 *  system.
 * ============================================================ */

static int enter(i32 x, i32 y, i32 w, i32 h) {
    if (!loaded || !app) return 0;
    if (app_faulted) { kapp_unload(); return 0; }
    cl_x = x; cl_y = y; cl_w = w; cl_h = h;
    /* Set the return point: if the application crashes, the exception
       handler brings control back here with a value of 1. */
    int rc;
    GUARD_SET(&guard_buf, rc);
    if (rc != 0) return 0;        /* guard_jump returns here after a fault */
    in_app = 1;
    return 1;
}
static void leave(void) {
    in_app = 0;
    if (app_faulted) {
        ksnprintf(last_error, sizeof(last_error), T("application terminated: %s", "приложение снято: %s"), fault_reason);
        kapp_unload();
    }
}

void kapp_draw(i32 x, i32 y, i32 w, i32 h) {
    if (!enter(x, y, w, h)) return;
    if (app->on_draw) app->on_draw();
    leave();
}

void kapp_tick(i32 x, i32 y, i32 w, i32 h) {
    if (!enter(x, y, w, h)) return;
    if (app->on_tick) app->on_tick();
    leave();
}

void kapp_key(int key, i32 x, i32 y, i32 w, i32 h) {
    if (!enter(x, y, w, h)) return;
    if (app->on_key) app->on_key(key);
    leave();
}

void kapp_click(i32 mx, i32 my, int button, i32 x, i32 y, i32 w, i32 h) {
    if (!enter(x, y, w, h)) return;
    if (app->on_click) app->on_click(mx, my, button);
    leave();
}

void kapp_opened(i32 x, i32 y, i32 w, i32 h) {
    if (!enter(x, y, w, h)) return;
    if (app->on_open) app->on_open();
    leave();
}
