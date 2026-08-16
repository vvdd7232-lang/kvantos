/* ============================================================
 *  KvantOS - загрузчик приложений .kapp и таблица системных вызовов
 *
 *  Как это работает:
 *    1. Файл читается с диска (KvFS) в буфер.
 *    2. Проверяется заголовок: подпись, версия ABI, размеры.
 *    3. Образ копируется по фиксированному адресу KAPP_LOAD_BASE,
 *       хвост bss обнуляется.
 *    4. Вызывается kapp_main(&api) - приложение возвращает описание
 *       себя с обработчиками событий.
 *    5. Оболочка дальше сама зовёт on_draw/on_key/on_click.
 *
 *  Приложение собрано под абсолютный адрес KAPP_LOAD_BASE, поэтому
 *  перемещать его не нужно - никаких таблиц релокаций. Плата за
 *  простоту: одновременно запущено может быть одно приложение.
 *  Для системы такого размера это честный размен.
 *
 *  Приложение работает в кольце ядра. Чтобы кривая программа не
 *  утащила за собой всю систему, вокруг каждого её вызова стоит
 *  «страховочная сетка»: обработчик исключений запоминает, что
 *  сбой произошёл внутри приложения, и вместо паники ядра
 *  выгружает виновника (см. kapp_guard_* ниже).
 * ============================================================ */
#include "kernel.h"
#include "kvapp.h"

/* ---------- состояние загруженного приложения ---------- */
static int        loaded = 0;
static kv_app_t  *app    = NULL;
static char       app_file[40];
static char       app_title[64];
static char       last_error[96];

/* Клиентская область, куда рисует приложение. Оболочка выставляет
   её перед каждым вызовом обработчика: приложение считает, что
   начало координат - левый верхний угол его окна. */
static i32 cl_x, cl_y, cl_w, cl_h;

/* ============================================================
 *  Страховочная сетка
 *
 *  Приложение работает в кольце ядра, поэтому его ошибка - это
 *  исключение процессора, которое обычно means panic и остановка
 *  системы. Так вести себя нельзя: из-за кривой игры не должна
 *  умирать вся ОС.
 *
 *  Поэтому перед каждым входом в приложение мы запоминаем точку
 *  возврата (регистры esp, ebp, ebx, esi, edi и адрес продолжения),
 *  а обработчик исключений, увидев флаг in_app, не паникует, а
 *  прыгает обратно в эту точку - как longjmp в обычном Си.
 *  Приложение снимается, оболочка продолжает работать.
 * ============================================================ */
typedef struct { u32 esp, ebp, ebx, esi, edi, eip; } kapp_jmp_t;

static kapp_jmp_t    guard_buf;
static volatile int  in_app = 0;
static volatile int  app_faulted = 0;
static char          fault_reason[64];

/* Сохранение точки возврата.
 *
 *  Тонкость, на которой легко обжечься: сохранять регистры обязана
 *  не отдельная функция, а САМ вызывающий код. Если спрятать это
 *  в функцию, она запомнит esp своей собственной рамки, вернётся,
 *  рамка исчезнет - и прыжок обратно уйдёт в затёртый стек.
 *  Поэтому здесь макрос: он разворачивается прямо в теле enter(),
 *  чья рамка живёт всё время работы обработчика приложения.
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

/* Вернуться в сохранённую точку. Обратно уже не возвращается. */
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
    strncpy(fault_reason, why ? why : "исключение", sizeof(fault_reason));
    fault_reason[sizeof(fault_reason) - 1] = 0;
}

/* Вызывается обработчиком исключений вместо panic(), если сбой
   произошёл внутри приложения. Управление сюда не возвращается. */
void kapp_recover(const char *why) {
    kapp_note_fault(why);
    in_app = 0;
    guard_jump(&guard_buf);
}

/* ============================================================
 *  Реализация системных функций для приложений
 * ============================================================ */

static kv_i32 api_width(void)  { return cl_w; }
static kv_i32 api_height(void) { return cl_h; }

static kv_u32 api_rgb(kv_u8 r, kv_u8 g, kv_u8 b) { return fb_rgb(r, g, b); }

/* Прямоугольник в координатах окна, обрезанный по клиентской области.
   Именно здесь приложение теряет возможность рисовать поверх чужих
   окон и панели задач: всё лишнее отсекается до вызова fb_fill. */
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

/* Отрезок по Брезенхэму: без него любая диаграмма превращается
   в мучение из отдельных точек. */
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

/* Текст с обрезкой по правому краю окна: рисуем посимвольно и
   останавливаемся, когда очередной знак уже не помещается. */
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

/* Строку состояния приложение пишет в буфер, оболочка рисует её сама */
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
    if (ms > 1000) ms = 1000;      /* приложение не должно вешать систему писком */
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

/* Линейный конгруэнтный генератор: приложениям хватает,
   а тянуть что-то серьёзное в ядро незачем. */
static u32 rnd_state = 2463534242u;
static kv_u32 api_random(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state ^ (u32)timer_ticks();
}

static void api_log(const char *s) { gui_log(s); }

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
};

/* ============================================================
 *  Загрузка и выгрузка
 * ============================================================ */

const char *kapp_last_error(void) { return last_error; }
int         kapp_loaded(void)     { return loaded; }
const char *kapp_name(void)       { return loaded ? app_title : ""; }
const char *kapp_filename(void)   { return loaded ? app_file : ""; }

kv_i32 kapp_pref_w(void) { return (loaded && app && app->width  > 0) ? app->width  : 420; }
kv_i32 kapp_pref_h(void) { return (loaded && app && app->height > 0) ? app->height : 300; }

/* Проверка заголовка отдельной функцией: диагностика для пользователя
   важнее краткости - невнятное «не запускается» бесит больше всего. */
static int check_header(const kapp_header_t *h, u32 filesize) {
    if (h->magic[0] != KAPP_MAGIC0 || h->magic[1] != KAPP_MAGIC1 ||
        h->magic[2] != KAPP_MAGIC2 || h->magic[3] != KAPP_MAGIC3) {
        strncpy(last_error, "это не приложение KvantOS (нет подписи KAPP)", sizeof(last_error));
        return -1;
    }
    if (h->version != KAPP_FORMAT_VERSION) {
        ksnprintf(last_error, sizeof(last_error),
                  "формат файла версии %u, ядро понимает %u", h->version, KAPP_FORMAT_VERSION);
        return -1;
    }
    if (h->api_version != KV_API_VERSION) {
        ksnprintf(last_error, sizeof(last_error),
                  "приложение собрано под ABI %u, в системе ABI %u", h->api_version, KV_API_VERSION);
        return -1;
    }
    if (h->load_base != KAPP_LOAD_BASE) {
        strncpy(last_error, "неверный адрес загрузки в заголовке", sizeof(last_error));
        return -1;
    }
    if (h->code_size == 0 || h->code_size > filesize - h->header_size) {
        strncpy(last_error, "размер кода в заголовке не совпадает с файлом", sizeof(last_error));
        return -1;
    }
    if (h->code_size + h->bss_size > KAPP_MAX_SIZE) {
        strncpy(last_error, "приложение больше 2 МиБ", sizeof(last_error));
        return -1;
    }
    if (h->entry < KAPP_LOAD_BASE || h->entry >= KAPP_LOAD_BASE + h->code_size) {
        strncpy(last_error, "точка входа вне образа приложения", sizeof(last_error));
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

/* Загружает приложение из файла KvFS. 0 - успех, иначе см. kapp_last_error() */
int kapp_load(const char *filename) {
    last_error[0] = 0;
    app_faulted = 0;

    if (loaded) kapp_unload();

    /* Ищем приложение сначала на диске, затем в ramfs.
       Второе позволяет запускать программы, вложенные в загрузочный
       образ: на машине без размеченного диска это единственный
       способ вообще что-то запустить. */
    u32 fsize = kvfs_mounted() ? kvfs_size(filename) : 0;
    rfile_t *rf = fsize ? NULL : ramfs_find(filename);

    if (!fsize && !rf) {
        ksnprintf(last_error, sizeof(last_error), "файл '%s' не найден", filename);
        return -1;
    }
    if (!fsize) fsize = rf->size;

    if (fsize < sizeof(kapp_header_t)) {
        strncpy(last_error, "файл слишком мал для приложения", sizeof(last_error));
        return -1;
    }

    u8 *tmp = (u8 *)kmalloc(fsize);
    if (!tmp) {
        strncpy(last_error, "не хватает памяти для загрузки", sizeof(last_error));
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
        strncpy(last_error, "ошибка чтения файла", sizeof(last_error));
        return -1;
    }

    kapp_header_t hdr;
    memcpy(&hdr, tmp, sizeof(hdr));
    if (check_header(&hdr, (u32)got) < 0) { kfree(tmp); return -1; }

    /* Разворачиваем образ по фиксированному адресу */
    u8 *dst = (u8 *)KAPP_LOAD_BASE;
    memcpy(dst, tmp + hdr.header_size, hdr.code_size);
    if (hdr.bss_size) memset(dst + hdr.code_size, 0, hdr.bss_size);
    kfree(tmp);

    /* Сбрасываем кэш инструкций: мы только что записали код данными */
    __asm__ volatile("" ::: "memory");

    strncpy(app_file, filename, sizeof(app_file));
    app_file[sizeof(app_file) - 1] = 0;
    strncpy(app_title, hdr.name[0] ? hdr.name : filename, sizeof(app_title));
    app_title[sizeof(app_title) - 1] = 0;
    app_status[0] = 0;

    /* Вызов точки входа под страховкой */
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
        ksnprintf(last_error, sizeof(last_error), "сбой при запуске: %s", fault_reason);
        return -1;
    }
    if (!result) {
        strncpy(last_error, "kapp_main() вернула ноль", sizeof(last_error));
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
 *  Вызов обработчиков из оболочки
 *
 *  Каждый вход в приложение обрамляется установкой окна и флага
 *  in_app. Если внутри случится исключение, panic() увидит флаг,
 *  напишет об этом и выгрузит приложение вместо остановки системы.
 * ============================================================ */

static int enter(i32 x, i32 y, i32 w, i32 h) {
    if (!loaded || !app) return 0;
    if (app_faulted) { kapp_unload(); return 0; }
    cl_x = x; cl_y = y; cl_w = w; cl_h = h;
    /* Ставим точку возврата: если приложение упадёт, обработчик
       исключений вернёт управление сюда со значением 1. */
    int rc;
    GUARD_SET(&guard_buf, rc);
    if (rc != 0) return 0;        /* сюда возвращает guard_jump после сбоя */
    in_app = 1;
    return 1;
}
static void leave(void) {
    in_app = 0;
    if (app_faulted) {
        ksnprintf(last_error, sizeof(last_error), "приложение снято: %s", fault_reason);
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
