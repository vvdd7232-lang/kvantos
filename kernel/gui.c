/* ============================================================
 *  KvantOS - the graphical shell (KvantGUI)
 *  Desktop, windows, taskbar, mouse, mini-applications.
 *  Drawing goes into a back buffer that is then blitted to screen.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"
#include "filemgr.h"
#include "gui_store_policy.h"
#include "mcc.h"

#define MAX_WIN 8
#define TITLE_H 22
#define PANEL_H 28
#define REVERT_SECONDS 15   /* grace period to confirm a resolution change */
#define ICON_W  96
#define ICON_H  74

typedef struct {
    i32  x, y, w, h;
    char title[32];
    int  used, visible, minimized;
    int  app;                 /* which application is inside */
    u32  accent;
    i32  scroll;
} window_t;

static window_t wins[MAX_WIN];
static int      z_order[MAX_WIN];       /* z_order[0] is the bottom-most */
static int      win_count = 0;
static int      dragging = -1;
static i32      drag_dx, drag_dy;
static int      running = 0;
static int store_format_confirm;
static store_format_request_t store_format_request;
/* Acknowledged input state for bounded emulator acceptance tests. */
static volatile int gui_mouse_seen;
static u32     *backbuf = NULL;
static u32     *desktop_cache = NULL; /* wallpaper, logo and desktop icons */
static int      desktop_cache_valid = 0;

#define GUI_MAX_DIRTY 24
static fb_clip_rect_t dirty_rects[GUI_MAX_DIRTY];
static int dirty_count = 0;
static int dirty_all = 0;

/* Pending confirmation of a resolution change (the 15-second undo). */
static int      revert_armed = 0;
static vmode_t  revert_mode;
static u64      revert_deadline = 0;
static u32      scr_w, scr_h;

/* palette */
static u32 C_DESK1, C_DESK2, C_PANEL, C_PANEL2, C_WIN, C_WINBORDER;
static u32 C_TITLE, C_TITLE_IN, C_TEXT, C_TEXT_DIM, C_WHITE, C_BLACK;
static u32 C_ACCENT, C_GREEN, C_RED, C_YELLOW, C_CYAN, C_SHADOW;
static u32 C_FIELD, C_FIELD_ALT, C_FIELD_BORDER, C_SELECT, C_LINE, C_BUTTON;

enum { APP_ABOUT = 0, APP_SYSMON, APP_FILES, APP_TERM, APP_PAINT, APP_HELP, APP_SETTINGS, APP_EDIT,
       APP_STORE,      /* installing and launching programs */
       APP_DEV, APP_USER, APP_MATRIX };     /* the .kapp window, then the Matrix app */

/* Application names. A static initialiser cannot contain a function
   call, so both languages live in separate tables and app_name()
   returns the right string. */
static const char *app_names_en[] = {
    "About", "Monitor", "Files", "Terminal", "Paint", "Help", "Settings", "Notepad",
    "Programs", "C Workshop", "Application", "Matrix"
};
static const char *app_names_ru[] = {
    "О системе", "Монитор", "Файлы", "Терминал", "Рисование", "Справка", "Настройки", "Блокнот",
    "Программы", "C-мастерская", "Приложение", "Матрица"
};
static const char *app_name(int i)
{
    return kv_pick(app_names_en[i], app_names_ru[i]);
}

/* One-line explanations shown as desktop tooltips: a new user should
   not have to guess what an icon does. */
static const char *app_descs_en[] = {
    "What this system is", "Memory and tasks", "Disks and folders",
    "The command line", "Draw with the mouse", "How to use KvantGUI",
    "Screen and colours", "Notes and text files",
    "Install and run apps", "Write C programs", "A running program", "Falling green code"
};
static const char *app_descs_ru[] = {
    "Что это за система", "Память и задачи", "Диски и папки",
    "Командная строка", "Рисование мышью", "Как пользоваться",
    "Экран, цвета и язык", "Заметки и тексты",
    "Запуск программ", "Код и сборка на C", "Запущенная программа", "Падающий зелёный код"
};
static const char *app_desc(int i)
{
    return kv_pick(app_descs_en[i], app_descs_ru[i]);
}

static void app_matrix(window_t *v, i32 cx, i32 cy, i32 cw);

/* ---------- appearance settings, changed in Settings ---------- */
static int  theme_accent = 0;      /* accent colour index */
static int  theme_wall   = 4;      /* Matrix rain is the default desktop */
static int  opt_seconds  = 1;      /* show seconds in the clock */
static int  opt_shadows  = 1;      /* shadows under windows */

static const struct { u8 r, g, b; } accents[] = {
    {   0, 180,  70 },   /* Neon - the hacker-terminal default */
    {  38,  92, 168 },
    {  20, 180, 160 },
    {  32, 132,  84 },
    { 118,  62, 156 },
    { 166,  54,  70 },
    {  70,  78,  96 },
};
static const char *accent_names_en[] = {
    "Neon", "Blue", "Teal", "Emerald", "Purple", "Garnet", "Graphite"
};
static const char *accent_names_ru[] = {
    "Неон", "Синий", "Бирюзовый", "Изумруд", "Пурпур", "Гранат", "Графит"
};
static const char *accent_name(int i)
{
    return kv_pick(accent_names_en[i], accent_names_ru[i]);
}
#define ACCENT_COUNT ((int)(sizeof(accents) / sizeof(accents[0])))

static const char *wall_names_en[] = { "Gradient", "Night", "Grid", "Solid", "Matrix" };
static const char *wall_names_ru[] = { "Градиент", "Ночь", "Сетка", "Однотонный", "Матрица" };
static const char *wall_name(int i)
{
    return kv_pick(wall_names_en[i], wall_names_ru[i]);
}
#define WALL_COUNT 5

/* ---------- clickable areas of the current frame ---------- */
#define MAX_HIT 96
typedef struct { i32 x, y, w, h; int id; int win; } hit_t;
static hit_t hits[MAX_HIT];
static int   hit_count = 0;

static void hit_reset(void) { hit_count = 0; }

static void hit_add(int win, int id, i32 x, i32 y, i32 w, i32 h) {
    if (hit_count >= MAX_HIT) return;
    hits[hit_count++] = (hit_t){ x, y, w, h, id, win };
}

static int hit_test(int win, i32 mx, i32 my) {
    for (int i = hit_count - 1; i >= 0; i--) {
        hit_t *h = &hits[i];
        if (h->win != win) continue;
        if (mx >= h->x && mx < h->x + h->w && my >= h->y && my < h->y + h->h)
            return h->id;
    }
    return -1;
}

static void hit_remove_window(int win) {
    int out=0;
    for(int i=0;i<hit_count;i++) if(hits[i].win!=win) hits[out++]=hits[i];
    hit_count=out;
}

static int damage_touches(const fb_clip_rect_t *a, const fb_clip_rect_t *b) {
    return a->x <= b->x + b->w && b->x <= a->x + a->w &&
           a->y <= b->y + b->h && b->y <= a->y + a->h;
}

static void damage_full(void) {
    dirty_count = 0;
    dirty_all = 1;
}

static void damage_add_rect(i32 x, i32 y, i32 w, i32 h) {
    if (dirty_all || w <= 0 || h <= 0 || !scr_w || !scr_h) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (i32)scr_w || y >= (i32)scr_h || w <= 0 || h <= 0) return;
    if (w > (i32)scr_w - x) w = (i32)scr_w - x;
    if (h > (i32)scr_h - y) h = (i32)scr_h - y;
    if (w <= 0 || h <= 0) return;

    fb_clip_rect_t added = { x, y, w, h };
    /* Keep the list non-overlapping; a merged bounding box can touch a
       third rectangle, so re-check from the beginning after each merge. */
    for (int i = 0; i < dirty_count;) {
        if (!damage_touches(&added, &dirty_rects[i])) { i++; continue; }
        fb_clip_rect_t *r = &dirty_rects[i];
        i32 x0 = added.x < r->x ? added.x : r->x;
        i32 y0 = added.y < r->y ? added.y : r->y;
        i32 x1 = added.x + added.w > r->x + r->w ? added.x + added.w : r->x + r->w;
        i32 y1 = added.y + added.h > r->y + r->h ? added.y + added.h : r->y + r->h;
        added = (fb_clip_rect_t){ x0, y0, x1 - x0, y1 - y0 };
        for (int j = i; j < dirty_count - 1; j++) dirty_rects[j] = dirty_rects[j + 1];
        dirty_count--;
        i = 0;
    }
    if (dirty_count >= GUI_MAX_DIRTY) { damage_full(); return; }
    dirty_rects[dirty_count++] = added;

    u32 screen_area = scr_w * scr_h;
    u32 area = 0;
    for (int i = 0; i < dirty_count; i++) {
        u32 rarea = (u32)dirty_rects[i].w * (u32)dirty_rects[i].h;
        if (rarea >= screen_area - area) { area = screen_area; break; }
        area += rarea;
    }
    if (area > screen_area / 2) damage_full();
}

static void damage_panel(void) {
    damage_add_rect(0, (i32)scr_h - PANEL_H, (i32)scr_w, PANEL_H);
}

/* identifiers of the Settings controls */
#define W_TAB      100    /* + tab number   */
#define W_RES      200    /* + mode number  */
#define W_HZ       300    /* + rate number  */
#define W_ACCENT   400    /* + colour number */
#define W_WALL     500    /* + background number */
#define W_APPLY    600
#define W_SECONDS  601
#define W_SHADOWS  602
#define W_QUIT     603
#define W_AA       604    /* anti-aliased (smooth) text */

/* ---------- housekeeping ---------- */

static void palette_init(void) {
    C_DESK1     = fb_rgb(3, 12, 6);
    C_DESK2     = fb_rgb(0, 4, 1);
    C_TITLE     = fb_rgb(accents[theme_accent].r,
                         accents[theme_accent].g,
                         accents[theme_accent].b);
    C_PANEL     = fb_rgb(4, 13, 7);
    C_PANEL2    = fb_rgb(12, 34, 18);
    C_WIN       = fb_rgb(5, 12, 8);
    C_WINBORDER = fb_rgb(28, 86, 48);
    C_TITLE_IN  = fb_rgb(100, 210, 135);
    C_TEXT      = fb_rgb(125, 255, 165);
    C_TEXT_DIM  = fb_rgb(68, 145, 92);
    C_WHITE     = fb_rgb(228, 255, 236);
    C_BLACK     = fb_rgb(0, 0, 0);
    C_ACCENT    = fb_rgb(accents[theme_accent].r,
                         accents[theme_accent].g,
                         accents[theme_accent].b);
    C_GREEN     = fb_rgb(0, 255, 110);
    C_RED       = fb_rgb(255, 86, 86);
    C_YELLOW    = fb_rgb(255, 208, 84);
    C_CYAN      = fb_rgb(88, 255, 220);
    C_SHADOW    = fb_rgb(0, 4, 2);
    C_FIELD       = fb_rgb(8, 20, 12);
    C_FIELD_ALT   = fb_rgb(12, 31, 18);
    C_FIELD_BORDER= fb_rgb(30, 88, 50);
    C_SELECT      = fb_rgb(10, 62, 30);
    C_LINE        = fb_rgb(20, 62, 34);
    C_BUTTON      = fb_rgb(14, 40, 22);
}

static int top_window(void) {
    for (int i = win_count - 1; i >= 0; i--) {
        int id = z_order[i];
        if (wins[id].used && wins[id].visible && !wins[id].minimized) return id;
    }
    return -1;
}

static void raise_window(int id) {
    int pos = -1;
    for (int i = 0; i < win_count; i++) if (z_order[i] == id) { pos = i; break; }
    if (pos < 0) return;
    for (int i = pos; i < win_count - 1; i++) z_order[i] = z_order[i + 1];
    z_order[win_count - 1] = id;
}

static int win_open(const char *title, int app, i32 x, i32 y, i32 w, i32 h, u32 accent) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used) continue;
        window_t *v = &wins[i];
        memset(v, 0, sizeof(*v));
        v->used = v->visible = 1;
        v->x = x; v->y = y; v->w = w; v->h = h;
        v->app = app;
        v->accent = accent;
        strncpy(v->title, title, sizeof(v->title));
        z_order[win_count++] = i;
        return i;
    }
    return -1;
}

static void win_close(int id) {
    if (id < 0 || id >= MAX_WIN || !wins[id].used) return;
    if (wins[id].app == APP_USER) kapp_unload();
    if(wins[id].app==APP_FILES && fm_input_active())fm_input_cancel();
    if(wins[id].app==APP_STORE)store_format_confirm=0;
    hit_remove_window(id);
    wins[id].used = 0;
    int pos = -1;
    for (int i = 0; i < win_count; i++) if (z_order[i] == id) { pos = i; break; }
    if (pos >= 0) {
        for (int i = pos; i < win_count - 1; i++) z_order[i] = z_order[i + 1];
        win_count--;
    }
}

/* ---------- window contents ---------- */

static void draw_bar(i32 x, i32 y, i32 w, u32 pct, u32 col) {
    if (pct > 100) pct = 100;
    fb_fill(x, y, w, 12, C_FIELD);
    fb_fill(x, y, (i32)((u32)w * pct / 100u), 12, col);
    fb_rect(x, y, w, 12, C_FIELD_BORDER);
}

static void app_about(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 y = cy + 10;
    fb_text(cx + 12, y, "KvantOS " KV_VERSION, C_TEXT, 0xFFFFFFFF); y += 22;
    fb_fill(cx + 12, y, cw - 24, 1, C_LINE); y += 10;

    char line[96];
    ksnprintf(line, sizeof(line), T("Architecture: %s", "Архитектура: %s"), KV_ARCH);
    fb_text(cx + 12, y, line, C_TEXT, 0xFFFFFFFF); y += 18;
    fb_text(cx + 12, y, T("Bootloader: GRUB 2 (Multiboot 1)", "Загрузчик: GRUB 2 (Multiboot 1)"), C_TEXT, 0xFFFFFFFF); y += 18;
    fb_text(cx + 12, y, T("Kernel: monolithic, written from scratch", "Ядро: монолитное, собственное"), C_TEXT, 0xFFFFFFFF); y += 18;

    ksnprintf(line, sizeof(line), T("Video mode: %ux%u, %u bpp", "Видеорежим: %ux%u, %u бит"),
              fb_width(), fb_height(), fb_bpp_get());
    fb_text(cx + 12, y, line, C_TEXT, 0xFFFFFFFF); y += 18;

    char vendor[16], brand[52];
    cpu_vendor(vendor); cpu_brand(brand);
    ksnprintf(line, sizeof(line), "CPU: %s", vendor);
    fb_text(cx + 12, y, line, C_TEXT, 0xFFFFFFFF); y += 18;
    fb_text(cx + 12, y, brand, C_TEXT_DIM, 0xFFFFFFFF); y += 22;

    fb_text(cx + 12, y, T("Built:", "Собрано:"), C_TEXT_DIM, 0xFFFFFFFF);
    fb_text(cx + 12 + 72, y, KV_BUILD, C_TEXT, 0xFFFFFFFF);
}

static void app_sysmon(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 y = cy + 10;
    char line[96];

    u32 total = pmm_total_bytes();
    u32 used  = pmm_used_frames() * 4096;
    u32 pct   = total ? (used / 1024) * 100u / (total / 1024) : 0;

    fb_text(cx + 12, y, T("Physical memory", "Физическая память"), C_TEXT, 0xFFFFFFFF); y += 20;
    ksnprintf(line, sizeof(line), T("%u of %u MiB (%u%%)", "%u из %u МиБ (%u%%)"),
              used / 1048576, total / 1048576, pct);
    fb_text(cx + 12, y, line, C_TEXT_DIM, 0xFFFFFFFF); y += 18;
    draw_bar(cx + 12, y, cw - 24, pct, C_GREEN); y += 26;

    u32 ht, hu, hb;
    heap_stats(&ht, &hu, &hb);
    fb_text(cx + 12, y, T("Kernel heap", "Куча ядра"), C_TEXT, 0xFFFFFFFF); y += 20;
    ksnprintf(line, sizeof(line), T("%u KiB of %u KiB, %u blocks", "%u КиБ из %u КиБ, блоков %u"), hu / 1024, ht / 1024, hb);
    fb_text(cx + 12, y, line, C_TEXT_DIM, 0xFFFFFFFF); y += 18;
    draw_bar(cx + 12, y, cw - 24, ht ? (hu / 64) * 100u / (ht / 64) : 0, C_ACCENT); y += 26;

    u32 s = timer_seconds();
    ksnprintf(line, sizeof(line), T("Uptime: %uh %um %us", "Работает: %u ч %u мин %u с"), s / 3600, (s / 60) % 60, s % 60);
    fb_text(cx + 12, y, line, C_TEXT, 0xFFFFFFFF); y += 18;
    ksnprintf(line, sizeof(line), T("PIT ticks: %u at %u Hz", "Тиков PIT: %u при %u Гц"), (u32)timer_ticks(), timer_hz());
    fb_text(cx + 12, y, line, C_TEXT_DIM, 0xFFFFFFFF); y += 24;

    ksnprintf(line, sizeof(line), T("Scheduler tasks (%u)", "Задачи планировщика (%u)"), task_count());
    fb_text(cx + 12, y, line, C_TEXT, 0xFFFFFFFF); y += 20;

    task_t *cur = task_current(), *t = cur;
    int guard = 12;
    do {
        const char *st = t->state == TASK_READY ? T("ready", "готова") :
                         t->state == TASK_SLEEPING ? T("sleeping", "спит") : T("stopped", "стоп");
        ksnprintf(line, sizeof(line), "#%u %s", t->id, t->name);
        fb_text(cx + 20, y, line, C_TEXT, 0xFFFFFFFF);
        fb_text(cx + 20 + 150, y, st,
                t->state == TASK_READY ? C_GREEN : C_TEXT_DIM, 0xFFFFFFFF);
        ksnprintf(line, sizeof(line), "%u", t->switches);
        fb_text(cx + 20 + 230, y, line, C_TEXT_DIM, 0xFFFFFFFF);
        y += 16;
        t = t->next;
    } while (t != cur && guard-- > 0 && y < v->y + v->h - 20);
}

/* ==================== Notepad (text editor) ==================== */

#include "gui_editor_state.h"

/* ============================================================
 *  The file manager
 *
 *  Two panes side by side over any mounted volume. The drawing is here
 *  because it needs the framebuffer primitives; the state and all the
 *  actual file operations live in kernel/filemgr.c.
 * ============================================================ */

/* hit-test identifiers of the toolbar and the panes */
#define W_FM_PANE0   700
#define W_FM_PANE1   701
#define W_FM_ROW0    1000        /* + pane*512 + row  */
#define W_FM_UP      710
#define W_FM_COPY    711
#define W_FM_DEL     712
#define W_FM_MKDIR   713
#define W_FM_REFRESH 714
#define W_FM_VOLUME  715
#define W_FM_VIEWCLS 716
#define W_FM_YES     717
#define W_FM_NO      718
#define W_FM_RENAME  719
#define W_FM_APPLY   720
#define W_FM_CANCEL  721

static i32 fm_rows_visible(window_t *v) {
    i32 body = v->h - TITLE_H - 34 - 30 - 26;   /* toolbar, path, footer */
    i32 n = body / 18;
    return n < 1 ? 1 : n;
}

/* Shorten a long name so it fits: "a very long...name.txt" */
static void fit_name(char *dst, u32 dstsz, const char *src, u32 maxchars) {
    u32 len = utf8_len(src);
    if (len <= maxchars) { strncpy(dst, src, dstsz); return; }

    /* copy maxchars-3 characters, respecting UTF-8 boundaries */
    u32 keep = maxchars > 4 ? maxchars - 3 : 1;
    u32 chars = 0, i = 0, o = 0;
    while (src[i] && chars < keep && o < dstsz - 4) {
        u8 c = (u8)src[i];
        u32 clen = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        for (u32 k = 0; k < clen && src[i]; k++) dst[o++] = src[i++];
        chars++;
    }
    dst[o] = 0;
    if (o + 3 < dstsz) { dst[o] = '.'; dst[o+1] = '.'; dst[o+2] = '.'; dst[o+3] = 0; }
}

static void human_size(char *dst, u32 dstsz, u32 bytes) {
    if (bytes < 1024)              ksnprintf(dst, dstsz, "%u B", bytes);
    else if (bytes < 1024u * 1024) ksnprintf(dst, dstsz, "%u.%u K", bytes / 1024, (bytes % 1024) * 10 / 1024);
    else                           ksnprintf(dst, dstsz, "%u.%u M", bytes / (1024*1024),
                                             (bytes % (1024*1024)) * 10 / (1024*1024));
}

/* one pane */
static void fm_draw_pane(window_t *v, int win, int idx, i32 px, i32 py, i32 pw, i32 ph) {
    int active = (fm_active_pane() == idx);
    i32 rows = fm_rows_visible(v);

    /* frame */
    fb_fill(px, py, pw, ph, C_FIELD);
    fb_rect(px, py, pw, ph, active ? C_ACCENT : C_FIELD_BORDER);
    if (active) fb_rect(px + 1, py + 1, pw - 2, ph - 2, C_ACCENT);

    /* the volume header: name, type, free space */
    const char *path = fm_pane_path(idx);
    u32 tk = 0, fk = 0;
    fm_pane_space(idx, &tk, &fk);      /* cached: see filemgr.c */

    fb_fill(px + 1, py + 1, pw - 2, 20, active ? C_ACCENT : C_FIELD_ALT);
    char head[80];
    fit_name(head, sizeof(head), path, (u32)((pw - 90) / 8));
    fb_text(px + 6, py + 3, head, active ? C_WHITE : C_TEXT, 0xFFFFFFFF);

    char sp[32];
    if (tk) {
        if (fk >= 1024) ksnprintf(sp, sizeof(sp), T("%u M free", "%u М своб"), fk / 1024);
        else            ksnprintf(sp, sizeof(sp), T("%u K free", "%u К своб"), fk);
        fb_text(px + pw - 6 - (i32)utf8_len(sp) * 8, py + 3, sp,
                active ? C_WHITE : C_TEXT_DIM, 0xFFFFFFFF);
    }

    hit_add(win, idx ? W_FM_PANE1 : W_FM_PANE0, px, py, pw, ph);

    /* the rows */
    int count  = fm_pane_count(idx);
    int scroll = fm_pane_scroll(idx);
    int sel    = fm_pane_sel(idx);

    i32 y = py + 23;
    i32 name_chars = (pw - 96) / 8;
    if (name_chars < 6) name_chars = 6;

    if (!count) {
        fb_text(px + 10, y + 6, fm_listing_current(idx)?T("(empty)", "(пусто)"):T("Listing unavailable; refresh", "Список недоступен; обновите"), C_TEXT_DIM, 0xFFFFFFFF);
    }

    for (int r = 0; r < rows; r++) {
        int i = scroll + r;
        if (i >= count) break;
        vfs_dirent_t *e = fm_pane_item(idx, i);
        if (!e) break;

        int is_sel = (i == sel);
        u32 bg = is_sel ? (active ? C_ACCENT : C_SELECT)
                        : ((r & 1) ? C_FIELD_ALT : C_FIELD);
        fb_fill(px + 2, y, pw - 4, 18, bg);

        u32 fg = is_sel && active ? C_WHITE : C_TEXT;

        /* a small icon: folder or sheet */
        if (e->is_dir) {
            fb_fill(px + 7, y + 4, 6, 2, is_sel && active ? C_WHITE : C_YELLOW);
            fb_fill(px + 7, y + 6, 13, 9, is_sel && active ? C_WHITE : C_YELLOW);
        } else {
            u32 ic = is_sel && active ? C_WHITE : C_TEXT_DIM;
            fb_fill(px + 8, y + 3, 10, 12, is_sel && active ? C_ACCENT : C_FIELD_ALT);
            fb_rect(px + 8, y + 3, 10, 12, ic);
            fb_fill(px + 10, y + 6, 6, 1, ic);
            fb_fill(px + 10, y + 9, 6, 1, ic);
        }

        char nm[96];
        fit_name(nm, sizeof(nm), e->name, (u32)name_chars);
        fb_text(px + 24, y + 1, nm, fg, 0xFFFFFFFF);

        /* the size, or <DIR> */
        char sz[24];
        if (e->is_dir) strncpy(sz, T("<DIR>", "<КАТ>"), sizeof(sz));
        else           human_size(sz, sizeof(sz), e->size);
        i32 sw = (i32)utf8_len(sz) * 8;
        fb_text(px + pw - 8 - sw, y + 1,
                sz, is_sel && active ? C_WHITE : C_TEXT_DIM, 0xFFFFFFFF);

        hit_add(win, W_FM_ROW0 + idx * 512 + i, px + 2, y, pw - 4, 18);
        y += 18;
    }

    /* the scrollbar, only when it is needed */
    if (count > rows) {
        i32 track_y = py + 23, track_h = rows * 18;
        fb_fill(px + pw - 5, track_y, 3, track_h, C_FIELD_ALT);
        i32 bar = track_h * rows / count;
        if (bar < 12) bar = 12;
        i32 bpos = (count - rows) ? (track_h - bar) * scroll / (count - rows) : 0;
        fb_fill(px + pw - 5, track_y + bpos, 3, bar, C_ACCENT);
    }

    /* how many entries are here */
    char cnt[48];
    ksnprintf(cnt, sizeof(cnt), T("%d item(s)", "объектов: %d"), count);
    fb_text(px + 6, py + ph - 17, cnt, C_TEXT_DIM, 0xFFFFFFFF);
}

/* the file preview, drawn over the panes */
static void fm_draw_view(window_t *v, int win, i32 cx, i32 cy, i32 cw, i32 ch) {
    i32 w = cw - 40, h = ch - 40;
    if (w < 240) w = 240;
    i32 x = cx + 20, y = cy + 20;

    fb_fill(x + 4, y + 4, w, h, fb_rgb(0, 0, 0));      /* a soft shadow */
    fb_fill(x, y, w, h, C_WIN);
    fb_rect(x, y, w, h, C_ACCENT);
    fb_fill(x, y, w, 22, C_ACCENT);

    char title[80];
    fit_name(title, sizeof(title), fm_view_title(), (u32)((w - 60) / 8));
    fb_text(x + 8, y + 3, title, C_WHITE, 0xFFFFFFFF);
    fb_text(x + w - 18, y + 3, "x", C_WHITE, 0xFFFFFFFF);
    hit_add(win, W_FM_VIEWCLS, x + w - 24, y, 24, 22);

    const char *data = fm_view_data();
    int len = fm_view_length();
    int skip = fm_view_scroll_pos();
    i32 ty = y + 28;
    i32 max_y = y + h - 22;
    i32 cols = (w - 20) / 8;

    if (fm_view_is_binary()) {
        /* a hex dump: 16 bytes per line */
        int line = 0;
        for (int off = 0; off < len && ty < max_y; off += 16, line++) {
            if (line < skip) continue;
            char row[96];
            int o = 0;
            o += ksnprintf_ret(row + o, sizeof(row) - o, "%04x  ", (u32)off);
            for (int k = 0; k < 16; k++) {
                if (off + k < len)
                    o += ksnprintf_ret(row + o, sizeof(row) - o, "%02x ", (u8)data[off + k]);
                else
                    o += ksnprintf_ret(row + o, sizeof(row) - o, "   ");
            }
            o += ksnprintf_ret(row + o, sizeof(row) - o, " ");
            for (int k = 0; k < 16 && off + k < len; k++) {
                u8 c = (u8)data[off + k];
                row[o++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            row[o] = 0;
            fb_text(x + 10, ty, row, C_TEXT, 0xFFFFFFFF);
            ty += 16;
        }
    } else {
        int line = 0;
        int i = 0;
        while (i < len && ty < max_y) {
            char row[160];
            int o = 0;
            while (i < len && data[i] != '\n' && o < (int)sizeof(row) - 1 && o < cols)
                { if (data[i] != '\r') row[o++] = data[i]; i++; }
            while (i < len && data[i] != '\n') i++;      /* drop the tail */
            if (i < len) i++;                             /* skip the newline */
            row[o] = 0;
            if (line >= skip) { fb_text(x + 10, ty, row, C_TEXT, 0xFFFFFFFF); ty += 16; }
            line++;
        }
    }

    char foot[96];
    ksnprintf(foot, sizeof(foot),
              T("%d bytes shown  -  Esc closes, arrows scroll",
                "показано %d байт  —  Esc закрыть, стрелки — прокрутка"), len);
    fb_fill(x + 1, y + h - 20, w - 2, 19, C_FIELD_ALT);
    fb_text(x + 8, y + h - 18, foot, C_TEXT_DIM, 0xFFFFFFFF);
    (void)v;
}

/* a modal question */
static void fm_draw_confirm(int win, i32 cx, i32 cy, i32 cw, i32 ch) {
    i32 w = 380, h = 120;
    if (w > cw - 20) w = cw - 20;
    i32 x = cx + (cw - w) / 2, y = cy + (ch - h) / 2;

    fb_fill(x + 4, y + 4, w, h, fb_rgb(0, 0, 0));
    fb_fill(x, y, w, h, C_WIN);
    fb_rect(x, y, w, h, C_RED);
    fb_fill(x, y, w, 22, C_RED);
    fb_text_center(x, y + 3, w, T("Confirm deletion", "Подтвердите удаление"), C_WHITE, 0xFFFFFFFF);

    char nm[80];
    fit_name(nm, sizeof(nm), fm_confirm_target(), (u32)((w - 30) / 8));
    fb_text_center(x, y + 38, w, nm, C_TEXT, 0xFFFFFFFF);
    fb_text_center(x, y + 56, w, T("This cannot be undone.", "Отменить это будет нельзя."),
                   C_TEXT_DIM, 0xFFFFFFFF);

    widget_chip(win, W_FM_YES, x + w / 2 - 110, y + h - 34, 100, 26, T("Delete", "Удалить"), 1);
    widget_chip(win, W_FM_NO,  x + w / 2 + 10,  y + h - 34, 100, 26, T("Cancel", "Отмена"), 0);
}

/* Mouse-first name entry, shared by rename and new-folder actions. */
static void fm_draw_input(int win,i32 cx,i32 cy,i32 cw,i32 ch) {
    i32 w=460,h=174;if(w>cw-20)w=cw-20;
    i32 x=cx+(cw-w)/2,y=cy+(ch-h)/2;
    fb_fill(x+4,y+4,w,h,C_BLACK);fb_fill(x,y,w,h,C_WIN);fb_rect(x,y,w,h,C_ACCENT);
    fb_fill(x,y,w,22,C_ACCENT);fb_text_center(x,y+3,w,fm_input_title(),C_WHITE,0xFFFFFFFF);
    char text[VFS_MAX_PATH];fit_name(text,sizeof(text),fm_input_original(),(u32)((w-24)/8));
    fb_text(x+12,y+32,text,C_TEXT_DIM,0xFFFFFFFF);
    u32 bg=fm_input_selected()?C_ACCENT:C_FIELD,fg=fm_input_selected()?C_WHITE:C_TEXT;
    fb_fill(x+12,y+54,w-24,28,bg);fb_rect(x+12,y+54,w-24,28,C_ACCENT);
    fit_name(text,sizeof(text),fm_input_text(),(u32)((w-36)/8));fb_text(x+18,y+60,text,fg,0xFFFFFFFF);
    fit_name(text,sizeof(text),fm_status_text(),(u32)((w-24)/8));
    fb_text(x+12,y+98,text,fm_status_colour()?fm_status_colour():C_TEXT_DIM,0xFFFFFFFF);
    widget_chip(win,W_FM_APPLY,x+w/2-110,y+h-36,100,26,T("Apply", "Применить"),1);
    widget_chip(win,W_FM_CANCEL,x+w/2+10,y+h-36,100,26,T("Cancel", "Отмена"),0);
}

static void app_files(window_t *v, i32 cx, i32 cy, i32 cw) {
    int win = (int)(v - wins);
    i32 ch = v->h - TITLE_H - 2;

    fm_init();

    /* the toolbar */
    i32 tx = cx + 8, ty = cy + 6;
    widget_chip(win, W_FM_UP,      tx,       ty, 46, 24, T("Up", "Вверх"), 0);
    widget_chip(win, W_FM_VOLUME,  tx + 52,  ty, 74, 24, T("Volume", "Том"), 0);
    widget_chip(win, W_FM_COPY,    tx + 132, ty, 84, 24, T("Copy", "Копировать"), 0);
    widget_chip(win, W_FM_MKDIR,   tx + 222, ty, 84, 24, T("Folder", "Каталог"), 0);
    widget_chip(win, W_FM_DEL,     tx + 312, ty, 84, 24, T("Delete", "Удалить"), 0);
    widget_chip(win, W_FM_RENAME, tx + 402, ty, 104, 24, T("Rename", "Переимен."), 0);
    widget_chip(win, W_FM_REFRESH, tx + 512, ty, 76, 24, T("Refresh", "Обновить"), 0);

    /* the two panes */
    i32 top = cy + 36;
    i32 body_h = ch - 36 - 26;
    i32 gap = 8;
    i32 pw = (cw - 16 - gap) / 2;

    fm_draw_pane(v, win, 0, cx + 8,           top, pw, body_h);
    fm_draw_pane(v, win, 1, cx + 8 + pw + gap, top, pw, body_h);

    /* the status line */
    i32 sy = cy + ch - 22;
    fb_fill(cx + 1, sy, cw - 2, 21, C_FIELD_ALT);

    u32 col=fm_status_colour();
    fb_text(cx+10,sy+3,fm_status_text(),col?col:C_TEXT_DIM,0xFFFFFFFF);

    if (fm_view_is_open())      fm_draw_view(v, win, cx, cy, cw, ch);
    if (fm_confirm_pending())   fm_draw_confirm(win, cx, cy, cw, ch);
    if(fm_input_active())fm_draw_input(win,cx,cy,cw,ch);
}

/* a mini terminal: shows the output of the last command */
#define TERM_ROWS 14
#define TERM_COLS 56
static char term_buf[TERM_ROWS][TERM_COLS + 1];
static int  term_row = 0;
static char term_input[TERM_COLS];
static int  term_len = 0;

/* A log for applications: the line lands in the Terminal window.
   Declared before term_putline, defined right after it. */
void gui_log(const char *s);

static void term_putline(const char *s) {
    if (term_row >= TERM_ROWS) {
        for (int i = 0; i < TERM_ROWS - 1; i++)
            memcpy(term_buf[i], term_buf[i + 1], TERM_COLS + 1);
        term_row = TERM_ROWS - 1;
    }
    strncpy(term_buf[term_row], s, TERM_COLS + 1);
    term_row++;
}

void gui_log(const char *s) { if (s) term_putline(s); }

static void term_exec(const char *cmd) {
    char out[TERM_COLS + 1];
    ksnprintf(out, sizeof(out), "kvant$ %s", cmd);
    term_putline(out);

    if (!strcmp(cmd, "help")) {
        term_putline(" help mem ps ls date uptime clear about");
    } else if (!strcmp(cmd, "mem")) {
        ksnprintf(out, sizeof(out), T(" RAM: %u/%u MiB, %u pages", " ОЗУ: %u/%u МиБ, страниц %u"),
                  pmm_used_frames() * 4096 / 1048576,
                  pmm_total_bytes() / 1048576, pmm_total_frames());
        term_putline(out);
    } else if (!strcmp(cmd, "ps")) {
        task_t *cur = task_current(), *t = cur;
        int g = 8;
        do {
            ksnprintf(out, sizeof(out), " #%u %s (%s)", t->id, t->name,
                      t->state == TASK_READY ? T("ready", "готова") :
                      t->state == TASK_SLEEPING ? T("sleeping", "спит") : T("stopped", "стоп"));
            term_putline(out);
            t = t->next;
        } while (t != cur && g-- > 0);
    } else if (!strcmp(cmd, "ls")) {
        rfile_t *tbl = ramfs_table();
        for (int i = 0; i < RAMFS_MAX_FILES; i++)
            if (tbl[i].used) {
                ksnprintf(out, sizeof(out), " %6u  %s", tbl[i].size, tbl[i].name);
                term_putline(out);
            }
    } else if (!strcmp(cmd, "date")) {
        rtc_time_t t; rtc_read(&t);
        ksnprintf(out, sizeof(out), " %02u:%02u:%02u  %02u.%02u.%u",
                  t.hour, t.min, t.sec, t.day, t.month, t.year);
        term_putline(out);
    } else if (!strcmp(cmd, "uptime")) {
        u32 s = timer_seconds();
        ksnprintf(out, sizeof(out), T(" %uh %um %us", " %u ч %u мин %u с"), s / 3600, (s / 60) % 60, s % 60);
        term_putline(out);
    } else if (!strcmp(cmd, "about")) {
        {   /* Literals cannot be concatenated at compile time here:
               the translation is chosen at run time. */
            char ab[96];
            ksnprintf(ab, sizeof(ab), " KvantOS %s%s", KV_VERSION,
                      T(" — custom kernel, GRUB, 32-bit", " — своё ядро, GRUB, 32 бита"));
            term_putline(ab);
        }
    } else if (!strcmp(cmd, "clear")) {
        term_row = 0;
        memset(term_buf, 0, sizeof(term_buf));
    } else if (cmd[0]) {
        ksnprintf(out, sizeof(out), T(" command '%s' not found", " команда '%s' не найдена"), cmd);
        term_putline(out);
    }
}

static void app_term(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 ch = v->y + v->h - cy - 8;
    fb_fill(cx + 6, cy + 6, cw - 12, ch - 6, fb_rgb(0, 6, 2));
    i32 y = cy + 12;
    for (int i = 0; i < term_row && i < TERM_ROWS; i++) {
        fb_text(cx + 12, y, term_buf[i],
                term_buf[i][0] == 'k' ? C_CYAN : C_TEXT_DIM, 0xFFFFFFFF);
        y += 16;
    }
    char prompt[TERM_COLS + 12];
    ksnprintf(prompt, sizeof(prompt), "kvant$ %s_", term_input);
    fb_text(cx + 12, y, prompt, C_GREEN, 0xFFFFFFFF);
}

/* the canvas for drawing with the mouse */
#define PAINT_W 44
#define PAINT_H 26
static u8 paint_grid[PAINT_H][PAINT_W];
static u8 paint_color = 5;   /* black by default */

static void app_paint(window_t *v, i32 cx, i32 cy, i32 cw) {
    static const u8 pal[6][3] = {
        {250,250,252},{226,78,78},{72,200,120},{64,156,255},{240,190,70},{30,32,40}
    };
    fb_text(cx + 10, cy + 6, T("Draw with the mouse. Colour:", "Рисуйте мышью. Цвет:"), C_TEXT_DIM, 0xFFFFFFFF);
    for (int i = 0; i < 6; i++) {
        i32 bx = cx + 180 + i * 22, by = cy + 4;
        fb_fill(bx, by, 18, 16, fb_rgb(pal[i][0], pal[i][1], pal[i][2]));
        fb_rect(bx, by, 18, 16, i == paint_color ? C_BLACK : C_FIELD_BORDER);
    }
    i32 gx = cx + 10, gy = cy + 26;
    fb_fill(gx - 2, gy - 2, PAINT_W * 8 + 4, PAINT_H * 8 + 4, C_FIELD_BORDER);
    for (int r = 0; r < PAINT_H; r++)
        for (int c = 0; c < PAINT_W; c++) {
            u8 v2 = paint_grid[r][c];
            fb_fill(gx + c * 8, gy + r * 8, 8, 8,
                    fb_rgb(pal[v2][0], pal[v2][1], pal[v2][2]));
        }
}

static const char *gui_gpu_name(void) {
    pci_dev_t *g = pci_gpu();
    return g ? pci_gpu_model(g->vendor, g->device) : T("not detected", "не обнаружена");
}

/* ---------- widgets ---------- */

/* A toggle chip: frame plus label, the active one filled with the accent */
static void widget_chip(int win, int id, i32 x, i32 y, i32 w, i32 h,
                        const char *label, int active) {
    u32 bg = active ? C_TITLE : C_BUTTON;
    u32 fg = active ? C_WHITE : C_TEXT;
    fb_round_fill(x, y, w, h, bg);
    if (!active) fb_rect(x, y, w, h, C_FIELD_BORDER);
    fb_text_center(x, y + (h - 16) / 2 + 1, w, label, fg, 0xFFFFFFFF);
    hit_add(win, id, x, y, w, h);
}

/* An on/off checkbox */
static void widget_check(int win, int id, i32 x, i32 y,
                         const char *label, int on) {
    fb_fill(x, y, 16, 16, on ? C_TITLE : C_FIELD);
    fb_rect(x, y, 16, 16, on ? C_TITLE : C_FIELD_BORDER);
    if (on) {                       /* the tick */
        for (int i = 0; i < 4; i++) fb_pixel((u32)(x + 4 + i), (u32)(y + 8 + i), C_WHITE);
        for (int i = 0; i < 6; i++) fb_pixel((u32)(x + 7 + i), (u32)(y + 11 - i), C_WHITE);
    }
    fb_text(x + 24, y, label, C_TEXT, 0xFFFFFFFF);
    hit_add(win, id, x, y, 220, 18);
}

static void section(i32 x, i32 y, i32 w, const char *title) {
    fb_text(x, y, title, C_TEXT, 0xFFFFFFFF);
    fb_fill(x, y + 18, w, 1, C_LINE);
}

/* ---------- the Settings application ---------- */

static int  set_tab = 0;                 /* 0 display, 1 appearance, 2 system */
static int  set_res_sel = -1;            /* a mode selected but not yet applied */
static u32  set_hz_sel = 0;
static char set_msg[72] = "";
static u32  set_msg_color = 0;
static u64  set_msg_until = 0;

static const u32 hz_list[] = { 50, 60, 70, 72, 75, 85, 100, 120 };
#define HZ_COUNT ((int)(sizeof(hz_list) / sizeof(hz_list[0])))

static void set_status(const char *text, u32 color) {
    strncpy(set_msg, text, sizeof(set_msg));
    set_msg_color = color;
    set_msg_until = timer_ticks() + timer_hz() * 4;   /* 4 seconds */
}

static void app_settings(window_t *v, i32 cx, i32 cy, i32 cw) {
    int id = -1;
    for (int i = 0; i < MAX_WIN; i++) if (&wins[i] == v) { id = i; break; }

    /* tabs */
    const char *tabs[] = { T("Display", "Экран"), T("Appearance", "Оформление"), T("System", "Система") };
    i32 tx = cx + 10, ty = cy + 8;
    for (int i = 0; i < 3; i++) {
        i32 tw = (i32)utf8_len(tabs[i]) * 8 + 24;
        widget_chip(id, W_TAB + i, tx, ty, tw, 24, tabs[i], set_tab == i);
        tx += tw + 6;
    }
    fb_fill(cx + 10, ty + 30, cw - 20, 1, C_LINE);

    i32 y = ty + 42;
    char line[96];

    if (set_tab == 0) {
        /* ---- the Display tab ---- */
        vmode_t cur;
        vbe_current(&cur);

        ksnprintf(line, sizeof(line), T("Current mode: %u x %u, %u bpp", "Текущий режим: %u x %u, %u бит"),
                  cur.width, cur.height, cur.bpp);
        fb_text(cx + 14, y, line, C_TEXT, 0xFFFFFFFF);
        y += 18;
        ksnprintf(line, sizeof(line), T("Adapter: %s", "Видеокарта: %s"), gui_gpu_name());
        fb_text(cx + 14, y, line, C_TEXT_DIM, 0xFFFFFFFF);
        y += 24;

        section(cx + 14, y, cw - 28, T("Screen resolution", "Разрешение экрана"));
        y += 26;

        if (!vbe_can_modeset()) {
            fb_text(cx + 14, y, T("This adapter cannot change mode", "Видеокарта не поддерживает смену режима"),
                    C_RED, 0xFFFFFFFF);
            y += 20;
        } else {
            u32 vram = vbe_vram_bytes();
            i32 bx = cx + 14, by = y;
            u32 n = vbe_mode_count();
            for (u32 i = 0; i < n; i++) {
                vmode_t m;
                vbe_mode_get(i, &m);
                u32 need = m.width * m.height * (m.bpp >> 3);
                if (vram && need > vram) continue;

                ksnprintf(line, sizeof(line), "%ux%u", m.width, m.height);
                if (m.bpp != 32) ksnprintf(line, sizeof(line), "%ux%u/%u", m.width, m.height, m.bpp);

                int is_cur = (m.width == cur.width && m.height == cur.height && m.bpp == cur.bpp);
                int sel = (set_res_sel >= 0) ? ((u32)set_res_sel == i) : is_cur;

                i32 bw = 108;
                if (bx + bw > cx + cw - 14) { bx = cx + 14; by += 30; }
                widget_chip(id, W_RES + (int)i, bx, by, bw, 24, line, sel);
                bx += bw + 6;
            }
            y = by + 34;
        }

        section(cx + 14, y, cw - 28, T("Refresh rate", "Частота обновления"));
        y += 26;
        {
            i32 bx = cx + 14;
            u32 cur_hz = set_hz_sel ? set_hz_sel : vbe_get_refresh();
            for (int i = 0; i < HZ_COUNT; i++) {
                ksnprintf(line, sizeof(line), T("%u Hz", "%u Гц"), hz_list[i]);
                widget_chip(id, W_HZ + i, bx, y, 66, 24, line, cur_hz == hz_list[i]);
                bx += 70;
                if (bx + 66 > cx + cw - 14) { bx = cx + 14; y += 30; }
            }
            y += 34;
        }

        widget_chip(id, W_APPLY, cx + 14, y, 150, 28, T("Apply", "Применить"), 1);
        y += 38;

    } else if (set_tab == 1) {
        /* ---- the Appearance tab ---- */
        section(cx + 14, y, cw - 28, T("Accent colour", "Цвет акцента"));
        y += 26;
        for (int i = 0; i < ACCENT_COUNT; i++) {
            i32 bx = cx + 14 + (i % 3) * 130;
            i32 by = y + (i / 3) * 32;
            u32 col = fb_rgb(accents[i].r, accents[i].g, accents[i].b);
            fb_round_fill(bx, by, 122, 26, col);
            if (theme_accent == i) {
                fb_rect(bx - 2, by - 2, 126, 30, C_TEXT);
                fb_rect(bx - 1, by - 1, 124, 28, C_WHITE);
            }
            fb_text_center(bx, by + 5, 122, accent_name(i), C_WHITE, 0xFFFFFFFF);
            hit_add(id, W_ACCENT + i, bx, by, 122, 26);
        }
        y += ((ACCENT_COUNT + 2) / 3) * 32 + 12;

        section(cx + 14, y, cw - 28, T("Desktop background", "Фон рабочего стола"));
        y += 26;
        for (int i = 0; i < WALL_COUNT; i++) {
            i32 bx = cx + 14 + (i % 2) * 190;
            i32 by = y + (i / 2) * 32;
            widget_chip(id, W_WALL + i, bx, by, 180, 26, wall_name(i), theme_wall == i);
        }
        y += ((WALL_COUNT + 1) / 2) * 32 + 14;

        section(cx + 14, y, cw - 28, T("Other", "Прочее"));
        y += 26;
        widget_check(id, W_SHADOWS, cx + 14, y, T("Window shadows", "Тени под окнами"), opt_shadows);
        y += 26;
        widget_check(id, W_SECONDS, cx + 14, y, T("Seconds in the clock", "Секунды в часах"), opt_seconds);
        y += 26;
        widget_check(id, W_AA, cx + 14, y, T("Smooth (anti-aliased) text", "Сглаженный текст"), fb_aa_on());
        y += 26;

    } else {
        /* ---- the System tab ---- */
        section(cx + 14, y, cw - 28, T("Details", "Сведения"));
        y += 26;

        ksnprintf(line, sizeof(line), "%s %s", KV_NAME, KV_VERSION);
        fb_text(cx + 14, y, line, C_TEXT, 0xFFFFFFFF); y += 18;
        fb_text(cx + 14, y, KV_ARCH, C_TEXT_DIM, 0xFFFFFFFF); y += 18;

        char brand[52];
        cpu_brand(brand);
        fb_text(cx + 14, y, brand, C_TEXT_DIM, 0xFFFFFFFF); y += 24;

        u32 total = pmm_total_bytes(), used = pmm_used_frames() * 4096;
        ksnprintf(line, sizeof(line), T("Memory: %u of %u MiB", "Память: %u из %u МиБ"),
                  used / 1048576, total / 1048576);
        fb_text(cx + 14, y, line, C_TEXT, 0xFFFFFFFF); y += 18;

        u32 ht, hu, hb;
        heap_stats(&ht, &hu, &hb);
        ksnprintf(line, sizeof(line), T("Heap: %u of %u KiB, %u blocks", "Куча: %u из %u КиБ, блоков %u"),
                  hu / 1024, ht / 1024, hb);
        fb_text(cx + 14, y, line, C_TEXT_DIM, 0xFFFFFFFF); y += 18;

        u32 s = timer_seconds();
        ksnprintf(line, sizeof(line), T("Uptime: %uh %um %us", "Работает: %u ч %u мин %u с"),
                  s / 3600, (s / 60) % 60, s % 60);
        fb_text(cx + 14, y, line, C_TEXT_DIM, 0xFFFFFFFF); y += 18;

        ksnprintf(line, sizeof(line), T("Tasks: %u, PCI devices: %u", "Задач: %u, устройств PCI: %u"),
                  task_count(), pci_count());
        fb_text(cx + 14, y, line, C_TEXT_DIM, 0xFFFFFFFF); y += 28;

        section(cx + 14, y, cw - 28, T("Session", "Сеанс"));
        y += 26;
        widget_chip(id, W_QUIT, cx + 14, y, 210, 28, T("Exit to the kvsh console", "Выйти в консоль kvsh"), 0);
        y += 38;
    }

    /* the window status line (clipped to the client area width) */
    if (set_msg[0] && timer_ticks() < set_msg_until) {
        u32 maxch = (u32)(cw - 28) / 8;
        char clipped[72];
        const char *src = set_msg;
        u32 n = 0, bpos = 0;
        while (*src && n < maxch && bpos < sizeof(clipped) - 4) {
            const char *st = src;
            utf8_next(&src);
            u32 bl = (u32)(src - st);
            if (bpos + bl >= sizeof(clipped) - 1) break;
            memcpy(clipped + bpos, st, bl);
            bpos += bl; n++;
        }
        clipped[bpos] = 0;
        fb_text(cx + 14, v->y + v->h - 26, clipped, set_msg_color, 0xFFFFFFFF);
    }
    else if (set_msg[0] && timer_ticks() >= set_msg_until)
        set_msg[0] = 0;
}

/* Drawing the Notepad window. */
#include "gui_editor_view.h"

/* ============================================================
 *  Programs - installing and launching .kapp applications
 *
 *  Shows which disk was found, whether it is formatted, and the list
 *  of installed programs. They are launched and removed from here too.
 * ============================================================ */
static char store_msg[160];
static u64  store_msg_until = 0;

static void store_say(const char *m) {
    strncpy(store_msg, m, sizeof(store_msg));
    store_msg[sizeof(store_msg) - 1] = 0;
    store_msg_until = timer_ticks() + timer_hz() * 4;
}

/* Explicit validation only: no writes, retries or unrelated USB/AHCI rescan. */
static void store_mount_check(void) {
    int r=kvfs_mount();
    if(!r && !vfs_find("kv"))kvfs_vfs_register();
    if(!r)store_say(T("KvFS checked and mounted", "KvFS проверена и подключена"));
    else if(r==-1)store_say(T("No boot ATA disk; no writes", "Нет загрузочного ATA диска; записи нет"));
    else if(r==-2)store_say(T("KvFS read I/O error; do not format", "Ошибка чтения KvFS; не форматируйте"));
    else if(r==-3)store_say(T("No KvFS signature; other data may exist", "Нет подписи KvFS; другие данные возможны"));
    else store_say(T("KvFS validation refused; no writes", "Проверка KvFS отказала; записи нет"));
}
static void store_format_apply(void) {
    store_format_confirm=0;
    int d=ata_boot_drive();
    if(!ata_present() || !store_format_valid(&store_format_request,d,d<0?0:ata_sectors(d),vfs_generation())) {
        store_say(T("Disk/namespace changed; format cancelled", "Диск/namespace изменился; формат отменён"));return;
    }
    int r=kvfs_format();
    if(!r && !vfs_find("kv"))kvfs_vfs_register();
    store_say(!r?T("KvFS formatted", "KvFS размечена"):T("Format failed; disk state uncertain", "Ошибка форматирования; состояние диска неизвестно"));
}
static void store_draw_format(int win,i32 cx,i32 cy,i32 cw,i32 ch) {
    i32 w=460,h=188;if(w>cw-20)w=cw-20;
    i32 x=cx+(cw-w)/2,y=cy+(ch-h)/2;
    fb_fill(x+4,y+4,w,h,C_BLACK);fb_fill(x,y,w,h,C_WIN);fb_rect(x,y,w,h,C_RED);
    fb_fill(x,y,w,22,C_RED);fb_text_center(x,y+3,w,T("Confirm destructive format", "Подтвердите стирание"),C_WHITE,0xFFFFFFFF);
    char disk[96];ksnprintf(disk,sizeof(disk),T("Boot ATA disk %u (%u MiB)", "Загрузочный ATA диск %u (%u МиБ)"),(u32)store_format_request.drive,store_format_request.sectors>>11);
    fb_text_center(x,y+35,w,disk,C_TEXT,0xFFFFFFFF);
    fb_text_center(x,y+60,w,T("All KvFS file records will be erased.", "Все записи файлов KvFS будут стёрты."),C_RED,0xFFFFFFFF);
    fb_text_center(x,y+83,w,T("This is NOT a recovery operation.", "Это НЕ операция восстановления."),C_TEXT,0xFFFFFFFF);
    fb_text_center(x,y+104,w,T("Cancel unless erasing this disk is intended.", "Отмените, если не намерены стереть диск."),C_TEXT_DIM,0xFFFFFFFF);
    widget_chip(win,W_ST_FORMAT_YES,x+w/2-110,y+h-36,100,26,T("Erase", "Стереть"),1);
    widget_chip(win,W_ST_FORMAT_NO,x+w/2+10,y+h-36,100,26,T("Cancel", "Отмена"),0);
}

/* The list of applications that reached ramfs from the boot image.
   Needed on machines without a disk: programs can be run from there. */
static void store_apps_from_ram(window_t *v, int wid, i32 x, i32 y, i32 cw) {
    rfile_t *tbl = ramfs_table();
    int shown = 0;
    char line[96];

    fb_text(x, y, T("Applications from the boot image:", "Приложения из загрузочного образа:"), C_TEXT, 0xFFFFFFFF);
    y += 20;

    i32 list_h = v->h - TITLE_H - 40 - (y - v->y);
    if (list_h < 40) list_h = 40;
    fb_fill(x, y, cw - 24, list_h, C_FIELD);
    fb_rect(x, y, cw - 24, list_h, C_FIELD_BORDER);

    for (int i = 0; i < RAMFS_MAX_FILES && shown < (list_h - 8) / 20; i++) {
        if (!tbl[i].used) continue;
        u32 l = (u32)strlen(tbl[i].name);
        if (l < 6 || strcmp(tbl[i].name + l - 5, ".kapp")) continue;   /* .kapp files only */

        i32 iy = y + 4 + shown * 20;
        int running = kapp_loaded() && !strcmp(kapp_filename(), tbl[i].name);
        if (running) fb_fill(x + 2, iy - 1, cw - 28, 19, C_SELECT);

        fb_text(x + 8, iy, tbl[i].name, running ? C_ACCENT : C_TEXT, 0xFFFFFFFF);
        ksnprintf(line, sizeof(line), T("%u KiB", "%u КиБ"), (tbl[i].size + 1023) / 1024);
        fb_text(x + 270, iy, line, C_TEXT_DIM, 0xFFFFFFFF);
        fb_text(x + 350, iy, running ? T("running", "запущено") : T("run", "запустить"),
                running ? C_GREEN : C_ACCENT, 0xFFFFFFFF);
        hit_add(wid, W_ST_RAM0 + i, x + 2, iy - 1, 430, 19);
        shown++;
    }
    if (!shown)
        fb_text(x + 10, y + 10, T("No applications in the image.", "В образе приложений нет."), C_TEXT_DIM, 0xFFFFFFFF);
}

static void app_store(window_t *v, i32 cx, i32 cy, i32 cw) {
    int wid = (int)(v - wins);
    i32 x = cx + 12, y = cy + 10;
    char line[96];
    if(store_format_confirm){store_draw_format(wid,cx,cy,cw,v->h-TITLE_H-2);return;}

    /* --- disk information --- */
    if (!ata_present()) {
        fb_text(x, y, T("No disk found — installation unavailable.", "Диск не найден — установка недоступна."), C_YELLOW, 0xFFFFFFFF);
        y += 20;
        fb_text(x, y, T("The programs below run straight from the image.", "Программы ниже запускаются прямо из образа."), C_TEXT_DIM, 0xFFFFFFFF);
        y += 26;
        /* Applications are available even without a disk: they came
           from the boot image and live in ramfs. */
        store_apps_from_ram(v, wid, x, y, cw);
        return;
    }

    int d = ata_boot_drive();
    ksnprintf(line, sizeof(line), T("Disk: %s", "Диск: %s"), ata_model(d));
    fb_text(x, y, line, C_TEXT, 0xFFFFFFFF);
    y += 18;
    ksnprintf(line, sizeof(line), T("Size: %u MiB", "Объём: %u МиБ"), ata_size_mb(d));
    fb_text(x, y, line, C_TEXT_DIM, 0xFFFFFFFF);
    y += 22;

    /* Installing the system onto the hard disk. Shown only when we
       booted from media: otherwise there is nothing to write. */
    if (setup_available()) {
        fb_fill(x, y, cw - 24, 58, fb_rgb(32, 28, 6));
        fb_rect(x, y, cw - 24, 58, fb_rgb(120, 95, 20));
        fb_text(x + 8, y + 6, T("Install KvantOS onto this disk", "Установить KvantOS на этот диск"),
                C_YELLOW, 0xFFFFFFFF);

        fb_round_fill(x + 8, y + 26, 130, 24, C_ACCENT);
        fb_text(x + 20, y + 30, T("Install", "Установить"), C_WHITE, 0xFFFFFFFF);
        hit_add(wid, W_ST_SETUP, x + 8, y + 26, 130, 24);

        fb_round_fill(x + 148, y + 26, 190, 24, C_BUTTON);
        fb_text(x + 158, y + 30, T("Install, keeping files", "Установить, сохранив файлы"), C_TEXT, 0xFFFFFFFF);
        hit_add(wid, W_ST_SETUP_KEEP, x + 148, y + 26, 190, 24);
        y += 66;
    }

    if (!kvfs_mounted()) {
        fb_text(x,y,T("KvFS is not mounted.", "KvFS не подключена."),C_YELLOW,0xFFFFFFFF);y+=20;
        fb_text(x,y,T("Data may exist; formatting is not recovery.", "Данные могут быть; форматирование не восстановление."),C_TEXT_DIM,0xFFFFFFFF);y+=26;
        widget_chip(wid,W_ST_REFRESH,x,y,230,26,T("Check / mount KvFS", "Проверить / подключить"),0);
        widget_chip(wid,W_ST_FORMAT,x+238,y,240,26,T("Erase / format...", "Стереть / разметить..."),0);y+=34;
        if(store_msg[0] && timer_ticks()<store_msg_until)fb_text(x,y,store_msg,C_ACCENT,0xFFFFFFFF);
        y+=24;

        /* With no disk the programs still run straight from the image.
           They do not need installing - only saving files does. */
        store_apps_from_ram(v, wid, x, y, cw);
        return;
    }

    u32 mb, kb, nf;
    kvfs_stats(&mb, &kb, &nf);
    ksnprintf(line, sizeof(line), T("KvFS: %u files, %u KiB used", "KvFS: %u файлов, занято %u КиБ"), nf, kb);
    fb_text(x, y, line, C_GREEN, 0xFFFFFFFF);
    y += 24;

    fb_text(x, y, T("Installed programs:", "Установленные программы:"), C_TEXT, 0xFFFFFFFF);
    y += 20;

    /* --- the application list --- */
    i32 list_y = y;
    i32 list_h = cy + v->h - TITLE_H - 46 - list_y;
    if (list_h < 40) list_h = 40;
    fb_fill(x, list_y, cw - 24, list_h, C_FIELD);
    fb_rect(x, list_y, cw - 24, list_h, C_FIELD_BORDER);

    int shown = 0;
    char nm[44];
    u32 sz;
    int is_exec;
    for (int i = 0; i < 64 && shown < (list_h - 8) / 20; i++) {
        if (kvfs_list(i, nm, &sz, &is_exec) < 0) break;
        if (!is_exec) continue;                     /* only programs are listed */
        i32 iy = list_y + 4 + shown * 20;

        int running = kapp_loaded() && !strcmp(kapp_filename(), nm);
        if (running) fb_fill(x + 2, iy - 1, cw - 28, 19, C_SELECT);

        ksnprintf(line, sizeof(line), "%s", nm);
        fb_text(x + 8, iy, line, running ? C_ACCENT : C_TEXT, 0xFFFFFFFF);

        ksnprintf(line, sizeof(line), T("%u KiB", "%u КиБ"), (sz + 1023) / 1024);
        fb_text(x + 270, iy, line, C_TEXT_DIM, 0xFFFFFFFF);

        fb_text(x + 350, iy, running ? T("running", "запущено") : T("run", "запустить"),
                running ? C_GREEN : C_ACCENT, 0xFFFFFFFF);
        hit_add(wid, W_ST_ITEM0 + i, x + 2, iy - 1, 430, 19);

        fb_text(x + 450, iy, T("delete", "удалить"), C_RED, 0xFFFFFFFF);
        hit_add(wid, W_ST_DEL0 + i, x + 446, iy - 1, 70, 19);
        shown++;
    }
    if (!shown) {
        fb_text(x + 10, list_y + 10, T("Nothing installed yet.", "Пока ничего не установлено."), C_TEXT_DIM, 0xFFFFFFFF);
        fb_text(x + 10, list_y + 30, T("Build a sample from sdk/ and put it", "Соберите пример из sdk/ и запишите"), C_TEXT_DIM, 0xFFFFFFFF);
        fb_text(x + 10, list_y + 48, T("on the disk with the install command.", "на диск командой install."), C_TEXT_DIM, 0xFFFFFFFF);
    }

    /* --- bottom row --- */
    i32 by = cy + v->h - TITLE_H - 32;
    fb_round_fill(x, by, 96, 24, C_BUTTON);
    fb_text(x + 16, by + 4, T("Refresh", "Обновить"), C_TEXT, 0xFFFFFFFF);
    hit_add(wid, W_ST_REFRESH, x, by, 96, 24);

    if (store_msg[0] && timer_ticks() < store_msg_until)
        fb_text(x + 110, by + 4, store_msg, C_ACCENT, 0xFFFFFFFF);
    else if (kapp_loaded()) {
        ksnprintf(line, sizeof(line), T("Running: %s", "Работает: %s"), kapp_name());
        fb_text(x + 110, by + 4, line, C_TEXT_DIM, 0xFFFFFFFF);
    }
}

/* ============================================================
 *  The window of a running application
 *
 *  The shell hands the program the rectangle of its client area and
 *  calls its handlers. Everything else - the frame, the title, the
 *  status line - is drawn by the system so that all windows look
 *  the same.
 * ============================================================ */
static void app_user(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 ch = v->h - TITLE_H - 2;

    if (!kapp_loaded()) {
        fb_fill(cx, cy, cw, ch, C_WIN);
        fb_text(cx + 14, cy + 16, T("No application loaded.", "Приложение не загружено."), C_TEXT_DIM, 0xFFFFFFFF);
        const char *e = kapp_last_error();
        if (e && e[0]) fb_text(cx + 14, cy + 38, e, C_RED, 0xFFFFFFFF);
        return;
    }

    /* 20 pixels at the bottom are reserved for the status line */
    i32 sh = 20;
    i32 uh = ch - sh;
    if (uh < 20) uh = ch;

    /* The background is drawn by us: should the application forget to
       clear its canvas, the window would keep rubbish from the last
       frame. */
    fb_fill(cx, cy, cw, uh, C_WIN);
    kapp_tick(cx, cy, cw, uh);
    kapp_draw(cx, cy, cw, uh);

    if (uh != ch) {
        fb_fill(cx, cy + uh, cw, sh, C_FIELD_ALT);
        fb_fill(cx, cy + uh, cw, 1, C_LINE);
        const char *st = kapp_status();
        char trust[192];
        ksnprintf(trust,sizeof(trust),kapp_isolated()?T("ABI4/CPL3 | %s","ABI4/CPL3 | %s"):
                  T("Trusted/kernel | %s","Доверенное/ядро | %s"),st && st[0]?st:kapp_name());
        st=trust;
        fb_text(cx + 8, cy + uh + 2, (st && st[0]) ? st : kapp_name(),
                C_TEXT_DIM, 0xFFFFFFFF);
    }
}

static void app_help(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 y = cy + 10;
    fb_text_bold(cx + 12, y, T("KvantGUI controls", "Управление KvantGUI"), C_TEXT, 0xFFFFFFFF); y += 22;
    fb_fill(cx + 12, y, cw - 24, 1, C_LINE); y += 10;
    const char *rows[] = {
        T("Hover the pointer over an icon: a tooltip explains it.", "Наведите курсор на значок — появится подсказка."),
        T("Text too rough? Settings > Appearance > Smooth text.", "Текст грубый? Настройки > Внешний вид > Сглаженный текст."),
        T("Click an icon or the KVANT menu to open an app.", "Значок или меню КВАНТ — открыть приложение."),
        T("Drag title bars to move windows; X closes them.", "За заголовок — переместить; крестик — закрыть."),
        T("The bottom panel switches between windows.", "Панель снизу переключает окна."),
        T("Exit to console is in the KVANT menu.", "Выход в консоль — через меню КВАНТ."),
        T("Letters no longer launch or close windows.", "Буквы больше не открывают и не закрывают окна."),
        "",
        T("Notepad and C Workshop:", "Блокнот и C-мастерская:"),
        T("Click to position the text cursor.", "Щелчок ставит текстовый курсор."),
        T("Drag across text to select it.", "Протяжка мышью выделяет текст."),
        T("Use Copy / Cut / Paste / Undo / Redo buttons.", "Кнопки: Копия / Вырезать / Вставить / Отмена / Повтор."),
        T("Scroll with the wheel or draggable scrollbars.", "Колесо мыши и полосы прокручивают текст."),
        T("Open and Save as show clickable file dialogs.", "Открыть и Сохранить как — диалоги с файлами."),
        T("Unsaved changes require confirmation.", "Несохранённый текст защищён подтверждением."),
        T("Workshop: click Build, Run, Help.", "Мастерская: кнопки Собрать, Запуск, Справка."),
        T("Ctrl+C/V/X/A/Z/Y/S are optional shortcuts.", "Ctrl+C/V/X/A/Z/Y/S — необязательные сочетания."),
        "",
        T("Optional pointer control without a mouse:", "Если мыши нет — необязательный NumPad:"),
        T("Enable MouseKeys in KVANT or with NumLock.", "Включите мышь с NumPad в КВАНТ или NumLock."),
        T("8/2/4/6 move; 5 clicks; 0 holds; . releases.", "8/2/4/6 — ход; 5 — клик; 0 — зажать; . — отпустить."),
        T("Shift: precise; Ctrl: faster; +/-: speed.", "Shift — точно; Ctrl — быстро; +/- — скорость."),
    };
    for (u32 i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        fb_text(cx + 12, y, rows[i], rows[i][0] == ' ' ? C_TEXT_DIM : C_TEXT, 0xFFFFFFFF);
        y += 17;
    }
}

/* ---------- window rendering ---------- */

static void draw_window(int id, int active) {
    window_t *v = &wins[id];
    if (!v->used || !v->visible || v->minimized) return;

    /* soft shadow and a rounded, anti-aliased body */
    if (opt_shadows) fb_shadow(v->x, v->y, v->w, v->h, 8);
    fb_round_fill_aa(v->x, v->y, v->w, v->h, 8, C_WINBORDER);
    fb_round_fill_aa(v->x + 1, v->y + 1, v->w - 2, v->h - 2, 7, C_WIN);

    /* title bar with a gradient and rounded top corners */
    if (active) {
        u8 r = accents[theme_accent].r;
        u8 g = accents[theme_accent].g;
        u8 b = accents[theme_accent].b;
        fb_gradient_v_round(v->x + 1, v->y + 1, v->w - 2, TITLE_H, 7,
                            r, g, b,
                            (u8)(r * 3 / 4),
                            (u8)(g * 3 / 4),
                            (u8)(b * 3 / 4));
    } else {
        fb_gradient_v_round(v->x + 1, v->y + 1, v->w - 2, TITLE_H, 7,
                            24, 64, 38, 14, 38, 22);
    }
    fb_fill(v->x + 1, v->y + TITLE_H, v->w - 2, 1, C_WINBORDER);
    fb_text_bold(v->x + 10, v->y + 4, v->title, C_WHITE, 0xFFFFFFFF);

    /* round close button with an anti-aliased cross */
    i32 bx = v->x + v->w - 14, by = v->y + 12;
    fb_disc_aa(bx, by, 7, active ? C_RED : C_TEXT_DIM);
    fb_line_aa(bx - 3, by - 3, bx + 3, by + 3, C_WHITE);
    fb_line_aa(bx - 3, by + 3, bx + 3, by - 3, C_WHITE);

    /* client area */
    i32 cx = v->x + 1, cy = v->y + TITLE_H + 1, cw = v->w - 2;
    switch (v->app) {
        case APP_ABOUT:  app_about(v, cx, cy, cw); break;
        case APP_SYSMON: app_sysmon(v, cx, cy, cw); break;
        case APP_FILES:  app_files(v, cx, cy, cw); break;
        case APP_TERM:   app_term(v, cx, cy, cw); break;
        case APP_PAINT:  app_paint(v, cx, cy, cw); break;
        case APP_HELP:   app_help(v, cx, cy, cw); break;
        case APP_SETTINGS: app_settings(v, cx, cy, cw); break;
        case APP_EDIT:   app_edit(v, cx, cy, cw); break;
        case APP_DEV:    app_dev(v, cx, cy, cw); break;
        case APP_MATRIX: app_matrix(v, cx, cy, cw); break;
        case APP_STORE:  app_store(v, cx, cy, cw); break;
        case APP_USER:   app_user(v, cx, cy, cw); break;
    }
}

/* ---------- desktop ---------- */

/* The caption under an icon comes from app_name(): that way it
   follows the chosen language while the table stays static. */
typedef struct { i32 x, y; int app; } icon_t;
static icon_t icons[] = {
    { 30,  50, APP_ABOUT    },
    { 30, 140, APP_SYSMON   },
    { 30, 230, APP_FILES    },
    { 30, 320, APP_TERM     },
    { 30, 365, APP_DEV      },
    { 30, 410, APP_PAINT    },
    { 30, 500, APP_SETTINGS },
    { 30, 590, APP_EDIT     },
    { 30, 680, APP_STORE    },
    { 30, 770, APP_HELP     },
    { 30, 860, APP_MATRIX   },
};
#define ICON_COUNT ((int)(sizeof(icons) / sizeof(icons[0])))

static int  sel_icon = -1;
static int  hover_icon = -1;   /* icon under the pointer (tooltip + pill) */

static int icon_at(i32 mx, i32 my) {
    for (int i = 0; i < ICON_COUNT; i++) {
        i32 x = icons[i].x - 6, y = icons[i].y - 6;
        if (mx >= x && mx < x + ICON_W && my >= y && my < y + ICON_H)
            return i;
    }
    return -1;
}

static void layout_icons(void) {
    i32 dh = (i32)scr_h - PANEL_H;
    /* Both the top margin (50) and the caption height under an icon
       (58) are included so the last row never meets the taskbar. */
    i32 usable = dh - 50 - 58;
    int per_col = usable / 90 + 1;
    if (per_col < 1) per_col = 1;
    for (int i = 0; i < ICON_COUNT; i++) {
        icons[i].x = 30 + (i / per_col) * (ICON_W + 20);
        icons[i].y = 50 + (i % per_col) * 90;
    }
}

static void draw_icon_glyph(int app, i32 x, i32 y) {
    /* 48x40 canvas of smooth shapes: rounded tiles, discs and AA lines */
    switch (app) {
        case APP_ABOUT:
            fb_round_fill_aa(x + 8, y, 20, 36, 6, C_CYAN);
            fb_disc_aa(x + 18, y + 8, 3, fb_rgb(20, 30, 50));
            fb_fill(x + 15, y + 15, 6, 14, fb_rgb(20, 30, 50));
            break;
        case APP_SYSMON:
            fb_round_fill_aa(x, y + 2, 36, 26, 5, fb_rgb(30, 40, 60));
            for (int i = 0; i < 7; i++) {
                i32 x0 = x + 4 + i * 5, x1 = x + 9 + i * 5;
                i32 y0 = y + 22 - (i * 7 + 3) % 15, y1 = y + 22 - (i * 7 + 8) % 15;
                fb_line_aa(x0, y0, x1, y1, C_GREEN);
            }
            fb_fill(x + 12, y + 30, 12, 4, fb_rgb(30, 40, 60));
            break;
        case APP_FILES:
            fb_round_fill_aa(x + 1, y + 5, 15, 6, 2, fb_rgb(232, 186, 92));
            fb_round_fill_aa(x, y + 9, 36, 24, 4, C_YELLOW);
            fb_fill(x + 3, y + 14, 30, 2, fb_rgb(200, 150, 40));
            break;
        case APP_DEV:
            fb_round_fill_aa(x, y + 2, 36, 30, 5, fb_rgb(18, 36, 58));
            fb_text_bold(x + 5, y + 10, "C{}", C_CYAN, 0xFFFFFFFF);
            break;
        case APP_TERM:
            fb_round_fill_aa(x, y + 2, 36, 30, 5, fb_rgb(18, 22, 30));
            fb_line_aa(x + 6, y + 10, x + 12, y + 15, C_GREEN);
            fb_line_aa(x + 12, y + 15, x + 6, y + 20, C_GREEN);
            fb_fill(x + 14, y + 22, 9, 3, C_GREEN);
            break;
        case APP_STORE:
            /* a box with a raised lid - "install a program" */
            fb_round_fill_aa(x + 2, y + 12, 32, 20, 4, fb_rgb(206, 158, 84));
            fb_round_fill_aa(x + 2, y + 6,  32, 9, 4, fb_rgb(232, 186, 112));
            fb_fill(x + 14, y + 6, 8, 26, fb_rgb(170, 126, 62));
            fb_fill(x + 6, y + 18, 24, 2, fb_rgb(150, 110, 54));
            break;
        case APP_USER:
            /* a window with a slider - "a running program" */
            fb_round_fill_aa(x + 2, y + 4, 32, 26, 4, C_WHITE);
            fb_fill(x + 4, y + 6, 28, 5, C_ACCENT);
            fb_line_aa(x + 7, y + 18, x + 29, y + 18, fb_rgb(150, 158, 175));
            fb_disc_aa(x + 20, y + 18, 4, C_ACCENT);
            fb_fill(x + 6, y + 24, 20, 3, fb_rgb(178, 186, 200));
            break;
        case APP_EDIT:
            /* a sheet of ruled paper with a pencil */
            fb_round_fill_aa(x + 4, y + 2, 26, 32, 3, C_WHITE);
            fb_line_aa(x + 9, y + 10, x + 25, y + 10, fb_rgb(150, 158, 175));
            fb_line_aa(x + 9, y + 16, x + 25, y + 16, fb_rgb(150, 158, 175));
            fb_line_aa(x + 9, y + 22, x + 20, y + 22, fb_rgb(150, 158, 175));
            fb_disc_aa(x + 26, y + 27, 6, C_YELLOW);        /* pencil */
            break;
        case APP_PAINT:
            fb_round_fill_aa(x + 2, y + 4, 32, 26, 4, C_WHITE);
            fb_disc_aa(x + 10, y + 13, 4, C_RED);
            fb_disc_aa(x + 20, y + 19, 5, C_ACCENT);
            fb_disc_aa(x + 28, y + 11, 3, C_GREEN);
            break;
        case APP_SETTINGS: {
            u32 c = fb_rgb(190, 198, 214);
            fb_fill(x + 14, y + 3, 8, 30, c);       /* gear teeth, cross */
            fb_fill(x + 3, y + 14, 30, 8, c);
            fb_line_aa(x + 7, y + 7, x + 29, y + 29, c);
            fb_line_aa(x + 7, y + 29, x + 29, y + 7, c);
            fb_disc_aa(x + 18, y + 18, 12, c);      /* the gear body */
            fb_disc_aa(x + 18, y + 18, 5, fb_rgb(30, 40, 60));  /* the hole */
            break;
        }
        case APP_HELP:
            fb_round_fill_aa(x + 6, y + 2, 24, 32, 3, C_WHITE);
            fb_text_scaled(x + 13, y + 6, "?", 2, C_ACCENT, 0xFFFFFFFF);
            break;
        case APP_MATRIX:
            fb_round_fill_aa(x, y + 2, 36, 30, 5, fb_rgb(0, 8, 3));
            for (int i = 0; i < 6; i++) {
                i32 gx = x + 4 + i * 5;
                i32 gy = y + 5 + ((i * 7 + 3) % 20);
                fb_glyph(gx, gy, (u8)((i & 1) ? '1' : '0'),
                         i == 5 ? C_WHITE : C_GREEN, 0xFFFFFFFF);
            }
            break;
    }
}

/* Deterministic pseudo-random stream used by the rain wallpaper and app. */
static u32 matrix_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static const char matrix_chars[] = "0123456789ABCDEF<>/\\|+*#";

static u8 matrix_char(u32 seed, u32 row) {
    return (u8)matrix_chars[(matrix_hash(seed + row * 131u) >> 5) %
                            (sizeof(matrix_chars) - 1)];
}

/* A frozen curtain of green code for the cached desktop layer. */
static void draw_matrix_wall(i32 w, i32 h) {
    fb_fill(0, 0, w, h, fb_rgb(0, 4, 1));
    for (i32 x = 0; x < w; x += 14) {
        u32 seed = matrix_hash((u32)x * 2654435761u + 0x9e3779b9u);
        i32 length = 6 + (i32)(seed % 22);
        i32 head = (i32)((seed >> 8) % (u32)(h + length * 12)) - length * 12;
        for (i32 k = 0; k < length; k++) {
            i32 y = head + k * 12;
            if (y < 0 || y + 8 > h) continue;
            u32 g = (u32)(215 - k * 180 / length);
            u32 col = (k == 0) ? C_WHITE : fb_rgb(g / 4, g, g / 3);
            fb_glyph(x, y, matrix_char(seed, (u32)k), col, 0xFFFFFFFF);
        }
    }
}

/* Animated digital rain, built into the desktop shell. */
static void app_matrix(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 ch = v->y + v->h - cy - 10;
    if (ch < 24) ch = 24;
    fb_fill(cx, cy, cw, ch, fb_rgb(0, 4, 1));

    u32 now = (u32)timer_ticks();
    int cols = (cw - 16) / 12;
    if (cols > 96) cols = 96;
    for (int c = 0; c < cols; c++) {
        u32 seed = matrix_hash((u32)c * 2654435761u + 0x9e3779b9u);
        int length = 8 + (int)(seed % 20);
        u32 speed = 7 + ((seed >> 8) % 15);
        int range = ch / 12 + length;
        int head = (int)(((now / speed) + (seed >> 16)) % (u32)range) - length;
        for (int k = 0; k < length; k++) {
            int row = head + k;
            i32 y = cy + 4 + row * 12;
            if (y < cy || y + 8 > cy + ch) continue;
            u32 g = (u32)(230 - k * 190 / length);
            u32 col = (k == 0) ? C_WHITE : fb_rgb(g / 5, g, g / 4);
            fb_glyph(cx + 8 + c * 12, y,
                     matrix_char(seed, (u32)(row + (int)(now / 70))), col, 0xFFFFFFFF);
        }
    }

    const char *prompt = "root@kvant:~# ./matrix";
    i32 sy = cy + ch - 18;
    fb_fill(cx, sy, cw, 18, fb_rgb(0, 10, 4));
    fb_text(cx + 8, sy + 1, prompt, C_GREEN, 0xFFFFFFFF);
    if ((now / (timer_hz() / 2 + 1)) & 1)
        fb_fill(cx + 8 + (i32)strlen(prompt) * 8, sy + 2, 6, 13, C_GREEN);
}

static void draw_desktop(void) {
    i32 dh = (i32)scr_h - PANEL_H;
    u8 ar = accents[theme_accent].r;
    u8 ag = accents[theme_accent].g;
    u8 ab = accents[theme_accent].b;

    switch (theme_wall) {
        case 1:      /* Night - a dark vertical gradient */
            fb_gradient_v(0, 0, (i32)scr_w, dh, 12, 14, 24, 2, 3, 8);
            break;
        case 2: {    /* Grid - lines in the accent colour */
            fb_fill(0, 0, (i32)scr_w, dh, fb_rgb(14, 18, 30));
            u32 g = fb_rgb((u8)(ar / 3 + 10), (u8)(ag / 3 + 12), (u8)(ab / 3 + 20));
            for (i32 x = 0; x < (i32)scr_w; x += 40) fb_fill(x, 0, 1, dh, g);
            for (i32 y = 0; y < dh; y += 40) fb_fill(0, y, (i32)scr_w, 1, g);
            break;
        }
        case 3:      /* Solid - a muted accent */
            fb_fill(0, 0, (i32)scr_w, dh,
                    fb_rgb((u8)(ar / 3), (u8)(ag / 3), (u8)(ab / 3 + 6)));
            break;
        case 4:      /* Matrix - green code rain */
            draw_matrix_wall((i32)scr_w, dh);
            break;
        default:     /* Gradient - a shade of the accent, top to bottom */
            fb_gradient_v(0, 0, (i32)scr_w, dh,
                          (u8)(ar / 2 + 8), (u8)(ag / 2 + 12), (u8)(ab / 2 + 24),
                          8, 12, 24);
    }

    /* the logo in the centre: big, smooth letters */
    i32 ly = (i32)scr_h / 2 - 90;
    const char *logo = "K V A N T";
    i32 lw = (i32)utf8_len(logo) * 8 * 3;
    if (theme_wall == 4) {
        i32 bw = lw + 90, bh = 138;
        if (bw > (i32)scr_w - 20) bw = (i32)scr_w - 20;
        i32 bx = (i32)scr_w / 2 - bw / 2;
        fb_fill(bx, ly - 20, bw, bh, fb_rgb(0, 8, 3));
        fb_rect(bx, ly - 20, bw, bh, C_FIELD_BORDER);
    }
    fb_text_scaled((i32)scr_w / 2 - lw / 2 + 2, ly + 2, logo, 3,
                   fb_rgb(0, 5, 2), 0xFFFFFFFF);           /* soft logo shadow */
    fb_text_scaled((i32)scr_w / 2 - lw / 2, ly, logo, 3,
                   theme_wall == 4 ? C_GREEN
                   : fb_rgb((u8)(ar / 2 + 90), (u8)(ag / 2 + 95), (u8)(ab / 2 + 110)),
                   0xFFFFFFFF);
    const char *sub = T("desktop environment", "графическая среда");
    fb_text_center(0, ly + 56, (i32)scr_w, sub,
                   theme_wall == 4 ? C_TEXT_DIM
                   : fb_rgb((u8)(ar / 2 + 40), (u8)(ag / 2 + 45), (u8)(ab / 2 + 60)),
                   0xFFFFFFFF);
    fb_text_center(0, ly + 76, (i32)scr_w, KV_VERSION,
                   theme_wall == 4 ? fb_rgb(40, 120, 65)
                   : fb_rgb((u8)(ar / 2 + 20), (u8)(ag / 2 + 25), (u8)(ab / 2 + 40)),
                   0xFFFFFFFF);

    /* a first-run hint so the desktop explains itself */
    fb_text_center(0, dh - 26, (i32)scr_w,
                   T("Hover an icon for a hint - click to open, KVANT (bottom left) lists everything",
                     "Наведите курсор на значок - появится подсказка; КВАНТ слева внизу - все программы"),
                   C_TEXT_DIM, 0xFFFFFFFF);

    /* Icon placement depends only on screen geometry; the cache keeps
       this whole layer until the selection, language, theme or mode changes. */
    layout_icons();
    for (int i = 0; i < ICON_COUNT; i++) {
        i32 x = icons[i].x, y = icons[i].y;
        if (i == sel_icon)
            fb_round_fill(x - 6, y - 6, ICON_W, ICON_H, C_SELECT);
        draw_icon_glyph(icons[i].app, x + ICON_W / 2 - 24, y);
        if (i == sel_icon) {
            const char *n = app_name(icons[i].app);
            i32 tw = (i32)utf8_len(n) * 8;
            fb_text_bold(x - 6 + (ICON_W - tw) / 2, y + 44, n, C_WHITE, 0xFFFFFFFF);
        } else {
            fb_text_center(x - 6, y + 44, ICON_W, app_name(icons[i].app), C_WHITE, 0xFFFFFFFF);
        }
    }
}

static u32 gui_fps = 0;       /* frames per second, for measurements */
static u32 gui_fps_limit = 60; /* smooth default without spinning at unlimited FPS */

/* .kapp callbacks may animate their scene, so keep rendering those windows
   at the selected frame rate. Ordinary desktop windows are static and are
   refreshed only for input, once per second for the clock, or at 4 Hz for
   the live system monitor. */
static int gui_animated_window(void) {
    if (!kapp_loaded()) return -1;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].visible && !wins[i].minimized &&
            wins[i].app == APP_USER)
            return i;
    return -1;
}

static int gui_has_animated_window(void) {
    return gui_animated_window() >= 0;
}

static u32 gui_refresh_period(void) {
    u32 hz = timer_hz();
    if (!hz) return 1;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].visible && !wins[i].minimized) {
            if (wins[i].app == APP_SYSMON) {
                u32 quarter = hz / 4;
                return quarter ? quarter : 1;
            }
            if (wins[i].app == APP_MATRIX) {
                u32 eighth = hz / 8;
                return eighth ? eighth : 1;
            }
        }
    int top=top_window();
    if(top>=0 && (wins[top].app==APP_EDIT || wins[top].app==APP_DEV)) return hz/2+1;
    return hz;
}

static int launcher_open;
static void draw_launcher(void) {
    if(!launcher_open) return;
    i32 py=(i32)scr_h-PANEL_H,y=py-30-(ICON_COUNT+2)*26;
    fb_shadow(6,y,280,py-y,8);
    fb_round_fill_aa(6,y,280,py-y,8,C_ACCENT);
    fb_round_fill_aa(7,y+1,278,py-y-2,7,C_WIN);
    fb_gradient_v_round(7,y+1,278,28,7,
                        accents[theme_accent].r, accents[theme_accent].g, accents[theme_accent].b,
                        (u8)(accents[theme_accent].r * 3 / 4),
                        (u8)(accents[theme_accent].g * 3 / 4),
                        (u8)(accents[theme_accent].b * 3 / 4));
    fb_text_bold(18,y+7,T("Applications", "Приложения"),C_WHITE,0xFFFFFFFF);
    i32 lmx = mouse_x(), lmy = mouse_y();
    for(int i=0;i<ICON_COUNT;i++) {
        i32 row=y+30+i*26;
        if(lmx>=8 && lmx<284 && lmy>=row && lmy<row+26)
            fb_round_fill_aa(10,row+1,272,24,5,C_SELECT);
        fb_text(18,row+5,app_name(icons[i].app),C_TEXT,0xFFFFFFFF);
        /* keep the description inside the 280 px menu */
        char dbuf[80];
        fit_name(dbuf, sizeof(dbuf), app_desc(icons[i].app), 19);
        fb_text(118,row+5,dbuf,C_TEXT_DIM,0xFFFFFFFF);
        fb_fill(16,row+25,260,1,C_LINE);
    }
    fb_text(18,y+35+ICON_COUNT*26,kbd_mousekeys()?T("MouseKeys: ON", "Мышь с NumPad: ВКЛ"):
             T("MouseKeys: OFF", "Мышь с NumPad: ВЫКЛ"),C_ACCENT,0xFFFFFFFF);
    fb_text(18,y+35+(ICON_COUNT+1)*26,T("Exit to console", "Выйти в консоль"),C_RED,0xFFFFFFFF);
    fb_text(196,y+7,T("Esc closes", "Esc - закрыть"),C_TEXT_DIM,0xFFFFFFFF);
}

/* Tooltip chip geometry for icon i (also used for damage tracking) */
static void tooltip_rect(int i, i32 *tx, i32 *ty, i32 *tw) {
    const char *n = app_name(icons[i].app);
    const char *d = app_desc(icons[i].app);
    *tw = (i32)(utf8_len(n) + 3 + utf8_len(d)) * 8 + 16;
    *tx = icons[i].x + ICON_W + 4;
    *ty = icons[i].y + 4;
    if (*tx + *tw > (i32)scr_w - 4)
        *tx = icons[i].x - *tw - 10;
    if (*tx < 2) *tx = 2;
}

static void damage_tooltip(int i) {
    if (i < 0 || i >= ICON_COUNT) return;
    i32 tx, ty, tw;
    tooltip_rect(i, &tx, &ty, &tw);
    damage_add_rect(tx - 2, ty - 2, tw + 4, 28);
}

static void draw_tooltip(void) {
    /* the hover pill lives in the overlay, NOT in the cached desktop:
       invalidating that 3 MB cache on every pointer move would stall
       the whole GUI under a moving mouse */
    if (hover_icon >= 0 && !launcher_open) {
        i32 x = icons[hover_icon].x, y = icons[hover_icon].y;
        fb_round_fill(x - 6, y - 6, ICON_W, ICON_H, C_SELECT);
        draw_icon_glyph(icons[hover_icon].app, x + ICON_W / 2 - 24, y);
        const char *n = app_name(icons[hover_icon].app);
        i32 tw = (i32)utf8_len(n) * 8;
        fb_text_bold(x - 6 + (ICON_W - tw) / 2, y + 44, n, C_WHITE, 0xFFFFFFFF);
    }
    if (hover_icon < 0 || launcher_open) return;
    i32 tx, ty, tw;
    tooltip_rect(hover_icon, &tx, &ty, &tw);
    fb_shadow(tx, ty, tw, 24, 6);
    fb_round_fill_aa(tx, ty, tw, 24, 6, fb_rgb(0, 10, 4));
    const char *n = app_name(icons[hover_icon].app);
    fb_text_bold(tx + 8, ty + 4, n, C_WHITE, 0xFFFFFFFF);
    i32 off = (i32)utf8_len(n) * 8;
    char tail[96];
    ksnprintf(tail, sizeof(tail), " - %s", app_desc(icons[hover_icon].app));
    fb_text(tx + 8 + off, ty + 4, tail, C_TEXT_DIM, 0xFFFFFFFF);
}

static void draw_panel(void) {
    i32 py = (i32)scr_h - PANEL_H;
    fb_gradient_v(0, py, (i32)scr_w, PANEL_H, 6, 22, 12, 2, 8, 4);
    fb_fill(0, py, (i32)scr_w, 1, C_PANEL2);

    /* Confirmation banner for a just-applied resolution. Drawn centred
       near the top so it survives even if the desktop is unreadable. */
    if (revert_armed) {
        u64 now = timer_ticks();
        /* 64-bit division would pull in __udivdi3, which a freestanding
           kernel does not link: the remaining span fits in 32 bits. */
        u32 left = (now >= revert_deadline) ? 0
                 : (u32)(revert_deadline - now) / timer_hz() + 1;
        char b[96];
        ksnprintf(b, sizeof(b),
                  T("Keep this resolution? Enter - yes, Esc - undo (%u s)",
                    "Оставить это разрешение? Enter - да, Esc - отменить (%u с)"), left);
        i32 bw = (i32)(utf8_len(b) * 8) + 24;
        i32 bx0 = ((i32)scr_w - bw) / 2;
        if (bx0 < 0) bx0 = 0;
        fb_round_fill(bx0, 14, bw, 30, fb_rgb(90, 18, 18));
        fb_rect(bx0, 14, bw, 30, C_RED);
        fb_text(bx0 + 12, 22, b, C_WHITE, 0xFFFFFFFF);
    }

    /* the Start button */
    fb_round_fill(6, py + 4, 78, PANEL_H - 8, C_TITLE);
    fb_text_bold(16, py + 10, T("KVANT", "КВАНТ"), C_WHITE, 0xFFFFFFFF);

    /* window buttons */
    i32 bx = 94;
    for (int i = 0; i < win_count; i++) {
        int id = z_order[i];
        if (!wins[id].used) continue;
        int active = (id == top_window());
        fb_round_fill(bx, py + 4, 132, PANEL_H - 8,
                      active ? C_SELECT : C_BUTTON);
        /* clipped by UTF-8 characters so the text stays inside the button */
        char t[40];
        const char *src = wins[id].title;
        u32 nch = 0, bpos = 0;
        while (*src && nch < 14 && bpos < sizeof(t) - 4) {
            const char *st = src;
            utf8_next(&src);
            u32 blen = (u32)(src - st);
            if (bpos + blen >= sizeof(t) - 1) break;
            memcpy(t + bpos, st, blen);
            bpos += blen; nch++;
        }
        t[bpos] = 0;
        fb_text(bx + 8, py + 10, t, active ? C_WHITE : C_TEXT_DIM, 0xFFFFFFFF);
        bx += 138;
        if (bx > (i32)scr_w - 260) break;
    }

    /* the clock and statistics on the right */
    rtc_time_t tm;
    rtc_read(&tm);
    char clock[48];
    if (opt_seconds) ksnprintf(clock, sizeof(clock), "%02u.%02u  %02u:%02u:%02u", tm.day, tm.month, tm.hour, tm.min, tm.sec);
    else             ksnprintf(clock, sizeof(clock), "%02u.%02u  %02u:%02u", tm.day, tm.month, tm.hour, tm.min);
    /* right-aligned stack: clock+date, then RAM, then fps - no overlaps */
    i32 rx = (i32)scr_w - 14;
    i32 clw = (i32)utf8_len(clock) * 8;
    fb_text_bold(rx - clw, py + 10, clock, C_WHITE, 0xFFFFFFFF);
    rx -= clw + 18;

    char mem[32];
    ksnprintf(mem, sizeof(mem), T("RAM %u MiB", "ОЗУ %u МиБ"), pmm_used_frames() * 4096 / 1048576);
    i32 mw = (i32)utf8_len(mem) * 8;
    fb_text(rx - mw, py + 10, mem, C_CYAN, 0xFFFFFFFF);
    rx -= mw + 18;

    /* Show throughput only while an application is animating; a quiet,
       event-driven desktop is idle, not a one-frame-per-second lag. */
    char fps[24];
    int animating = gui_has_animated_window();
    if (animating) ksnprintf(fps, sizeof(fps), T("%u fps", "%u к/с"), gui_fps);
    else            ksnprintf(fps, sizeof(fps), "%s", T("idle", "простой"));
    i32 fwid = (i32)utf8_len(fps) * 8;
    fb_text(rx - fwid, py + 10, fps,
            animating && gui_fps >= 25 ? C_GREEN : C_TEXT_DIM,
            0xFFFFFFFF);
}

static void damage_window_rect(i32 x, i32 y, i32 w, i32 h) {
    if (w <= 0 || h <= 0) return;
    /* Include the soft shadow (up to 7 px around the body) as well. */
    damage_add_rect(x - 8, y - 8, w + 18, h + 18);
}

static void damage_window(const window_t *v) {
    if (!v || !v->used || !v->visible || v->minimized) return;
    damage_window_rect(v->x, v->y, v->w, v->h);
}

static int rect_intersects(const fb_clip_rect_t *r, i32 x, i32 y, i32 w, i32 h) {
    return w > 0 && h > 0 && r->x < x + w && x < r->x + r->w &&
           r->y < y + h && y < r->y + r->h;
}

static int window_intersects_dirty(const window_t *v) {
    if (!v || !v->used || !v->visible || v->minimized) return 0;
    for (int i = 0; i < dirty_count; i++)
        if (rect_intersects(&dirty_rects[i], v->x, v->y, v->w + 4, v->h + 4)) return 1;
    return 0;
}

static int dirty_intersects_panel(void) {
    i32 py = (i32)scr_h - PANEL_H;
    for (int i = 0; i < dirty_count; i++)
        if (rect_intersects(&dirty_rects[i], 0, py, (i32)scr_w, PANEL_H)) return 1;
    return 0;
}

static int dirty_intersects_banner(void) {
    if (!revert_armed) return 0;
    for (int i = 0; i < dirty_count; i++)
        if (rect_intersects(&dirty_rects[i], 0, 0, (i32)scr_w, 48)) return 1;
    return 0;
}

static void hit_prune_invisible(void) {
    int out=0;
    for(int i=0;i<hit_count;i++) {
        int id=hits[i].win;
        if(id>=0 && id<MAX_WIN && wins[id].used && wins[id].visible && !wins[id].minimized)
            hits[out++]=hits[i];
    }
    hit_count=out;
}

static void build_desktop_cache(void) {
    if (desktop_cache) { kfree(desktop_cache); desktop_cache = NULL; }
    desktop_cache_valid = 0;

    i32 desktop_h = (i32)scr_h - PANEL_H;
    if (desktop_h <= 0) return;
    u32 bytes = fb_pitch_get() * (u32)desktop_h;
    desktop_cache = (u32 *)kmalloc(bytes);
    if (!desktop_cache) return; /* correct, uncached redraw remains available */

    void *target = fb_get_target();
    fb_clear_clip();
    fb_set_target(desktop_cache);
    fb_clip_rect_t desk = { 0, 0, (i32)scr_w, desktop_h };
    fb_set_clip_rects(&desk, 1);
    draw_desktop();
    fb_clear_clip();
    fb_set_target(target);
    desktop_cache_valid = 1;
}

/* Reconstruct and present only the union of dirty rectangles. Each window
   is drawn once with a multi-rectangle framebuffer clip, so app callbacks
   still run once per frame even when the damage list is disjoint. */
static void render_damage(void) {
    if (!dirty_count && !dirty_all) return;
    int was_full = dirty_all;
    if (was_full) {
        dirty_rects[0] = (fb_clip_rect_t){ 0, 0, (i32)scr_w, (i32)scr_h };
        dirty_count = 1;
    }

    if (!desktop_cache_valid) build_desktop_cache();
    fb_set_target(backbuf);
    fb_set_clip_rects(dirty_rects, (u32)dirty_count);

    if (desktop_cache_valid) {
        i32 desktop_h = (i32)scr_h - PANEL_H;
        for (int i = 0; i < dirty_count; i++) {
            fb_clip_rect_t r = dirty_rects[i];
            if (r.y >= desktop_h) continue;
            if (r.y + r.h > desktop_h) r.h = desktop_h - r.y;
            fb_copy_rect(backbuf, desktop_cache, r.x, r.y, r.w, r.h);
        }
    } else {
        /* Low-memory fallback: redraw only the clipped part, not the screen. */
        draw_desktop();
    }

    if (was_full) hit_reset();
    else hit_prune_invisible();

    int top = top_window();
    for (int i = 0; i < win_count; i++) {
        int id = z_order[i];
        if (!window_intersects_dirty(&wins[id])) continue;
        if (!was_full) hit_remove_window(id);
        draw_window(id, id == top);
    }
    if (dirty_intersects_panel() || dirty_intersects_banner()) draw_panel();
    draw_launcher(); /* Same damage clip; always above windows. */
    draw_tooltip();  /* ...and the icon tooltip above everything else. */

    fb_clear_clip();
    for (int i = 0; i < dirty_count; i++) {
        fb_clip_rect_t r = dirty_rects[i];
        fb_present_rect(backbuf, r.x, r.y, r.w, r.h);
    }

    dirty_count = 0;
    dirty_all = 0;
}

/* the mouse cursor - an arrow with a tail */
static void draw_cursor(i32 mx, i32 my) {
    static const char *arrow[] = {
        "X           ",
        "XX          ",
        "X.X         ",
        "X..X        ",
        "X...X       ",
        "X....X      ",
        "X.....X     ",
        "X......X    ",
        "X.......X   ",
        "X........X  ",
        "X.....XXXXX ",
        "X..X..X     ",
        "X.X X..X    ",
        "XX  X..X    ",
        "X    X..X   ",
        "     X..X   ",
        "      XX    ",
    };
    for (int r = 0; r < 17; r++) {
        const char *row = arrow[r];
        for (int c = 0; row[c]; c++) {
            if (row[c] == 'X') fb_pixel((u32)(mx + c), (u32)(my + r), C_BLACK);
            else if (row[c] == '.') fb_pixel((u32)(mx + c), (u32)(my + r), C_WHITE);
        }
    }
}

/* ---------- input handling ---------- */

static void open_app(int app) {
    if (app == APP_EDIT && !note_editor.doc.lines) {
        text_reset(&note_editor.doc);
        strncpy(note_editor.name, "untitled.txt", sizeof(note_editor.name));
    }
    if (app == APP_DEV && !dev_initialized) {
        editor_t *previous = editor;
        editor = &dev_editor;
        dev_new();
        editor = previous;
        dev_initialized = 1;
    }
    /* if a window of this application already exists, raise it */
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].app == app) {
            wins[i].minimized = 0;
            raise_window(i);
            return;
        }
    i32 w = 420, h = 300;
    if (app == APP_SYSMON) { w = 430; h = 420; }
    if (app == APP_EDIT)   { w = 760; h = 540; }   /* the notepad needs room */
    if (app == APP_TERM)   { w = 480; h = 300; }
    if (app == APP_PAINT)  { w = 390; h = 290; }
    if (app == APP_HELP)   { w = 600; h = 440; }
    if (app == APP_SETTINGS) { w = 470; h = 430; }
    if (app == APP_STORE)  { w = 560; h = 420; }
    if (app == APP_FILES)  { w = 720; h = 480; }   /* two panes need width */
    if (app == APP_DEV) { w = 780; h = 560; }
    if (app == APP_MATRIX) { w = 560; h = 400; }
    if (app == APP_USER)   { w = kapp_pref_w() + 2; h = kapp_pref_h() + TITLE_H + 24; }
    if (w > (i32)scr_w - 20) w = (i32)scr_w - 20;
    if (h > (i32)scr_h - PANEL_H - 16) h = (i32)scr_h - PANEL_H - 16;
    i32 x = 190 + (win_count % 4) * 34;
    i32 y = 60 + (win_count % 4) * 28;
    if (x + w > (i32)scr_w) x = (i32)scr_w - w - 10;
    if (y + h > (i32)scr_h - PANEL_H) y = (i32)scr_h - PANEL_H - h - 8;
    win_open(app_name(app), app, x, y, w, h, C_TITLE);
}

/* Only one .kapp may run; image and disk launches have the same lifecycle. */
static void close_user_windows(void) {
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].app == APP_USER) win_close(i);
}

static int launch_user_app(const char *name, const char *message) {
    close_user_windows();
    if (kapp_load(name) != 0) {
        store_say(kapp_last_error());
        return 0;
    }
    open_app(APP_USER);
    int id = top_window();
    if (id < 0 || wins[id].app != APP_USER) {
        kapp_unload();
        store_say(T("No free window slots", "Нет свободного места для окна"));
        return 0;
    }
    {
        window_t *v = &wins[id];
        strncpy(v->title, kapp_name(), sizeof(v->title) - 1);
        kapp_opened(v->x + 1, v->y + TITLE_H + 1,
                    v->w - 2, v->h - TITLE_H - 22);
    }
    store_say(message);
    return 1;
}

/* The same source can be built without a disk; Program Manager sees the
   generated RAM .kapp immediately. KvFS receives a persistent copy if mounted. */
static const char dev_template[] =
    "// Click Build or Run. No #include needed.\n"
    "int count = 0;\n"
    "void draw(void) {\n"
    "    clear(rgb(24, 32, 50));\n"
    "    text(20, 20, \"My first C app\", rgb(230,240,255), -1);\n"
    "    text(20, 50, \"Click to move the square\", rgb(150,200,240), -1);\n"
    "    fill(20 + count * 12, 90, 40, 40, rgb(60,160,240));\n"
    "}\n"
    "void click(int x, int y, int button) {\n"
    "    count = (count + 1) % 20;\n"
    "}\n";

static void dev_new(void) {
    ed_reset();
    ed_name_mode = dev_help = 0;
    strncpy(ed_name, "hello.c", sizeof(ed_name));
    text_set(&editor->doc,dev_template,(u32)strlen(dev_template));
    ed_dirty=1; text_history_reset(&editor->doc,&editor->undo);
    ed_msg(T("Click Build or Run. Save to disk before reboot.", "Нажмите Собрать или Запуск. Сохраните на диск до перезапуска."));
}

static char dev_output[24];
static int dev_build(void) {
    static u8 binary[MCC_ISOLATED_OUTPUT_MAX];
    u32 size = 0, len = (u32)strlen(ed_name);
    if (len < 3 || len > 20 || strcmp(ed_name + len - 2, ".c")) {
        ed_msg(T("Name: up to 18 characters + .c", "Имя: до 18 символов и расширение .c")); return 0;
    }
    u32 source_size=ed_export();
    if(source_size>MCC_SOURCE_MAX) {
        char message[192];
        ksnprintf(message,sizeof(message),T("Source: %u bytes; compiler max %u. Text kept; editing/saving allowed.",
                   "Исходник: %u байт; компилятор до %u. Текст оставлен, редактирование и сохранение доступны."),source_size,MCC_SOURCE_MAX);
        ed_msg(message); return 0;
    }
    if (mcc_compile_target(editor_text, ed_name, binary, sizeof(binary), &size, dev_isolated) < 0) {
        char message[96];
        int row = mcc_error_line() - 1;
        ed_cy = row < 0 ? 0 : row >= ed_lines ? ed_lines - 1 : row;
        ed_cx = 0; editor->doc.selecting=0; ed_scroll_to_cursor();
        ksnprintf(message, sizeof(message), T("Line %d: %s", "Строка %d: %s"), mcc_error_line(), mcc_error());
        ed_msg(message); return 0;
    }
    /* Save source before publishing the binary; never run a stale disk image. */
    if (!ed_save()) return 0;
    memcpy(dev_output, ed_name, len - 2);
    strcpy(dev_output + len - 2, ".kapp");
    if (kvfs_mounted() && kvfs_write(dev_output, binary, size, 1) < 0) {
        ed_msg(T("Cannot write .kapp to disk", "Не удалось записать .kapp на диск")); return 0;
    }
    if (ramfs_write(dev_output, (const char *)binary, size) < 0) {
        ed_msg(T("Cannot write .kapp to RAM", "Не удалось записать .kapp в RAM")); return 0;
    }
    char message[96];
    ksnprintf(message, sizeof(message), T("%s: %u bytes, %s", "%s: %u байт, %s"),
              dev_output, size, kvfs_mounted() ? T("RAM + disk", "RAM и диск") : T("RAM only: temporary", "только RAM: временно"));
    ed_msg(message); return 1;
}

static void dev_run(void) {
    if (!dev_build()) return;
    if (!launch_user_app(dev_output, T("Built and started", "Собрано и запущено")))
        ed_msg(store_msg);
}

/* Apply the selected resolution straight from the desktop.
   The back buffer is tied to the old row pitch, so it must be released
   BEFORE the mode change and allocated again for the new size. */
/* Switch to a mode AND give the desktop a backbuffer for it.
   Returns VBE_OK only when both succeeded; on any failure the previous
   mode stays active and remains backed by a buffer. Never leaves the
   desktop rendering straight into video memory. */
static int mode_switch_buffered(const vmode_t *want, const vmode_t *fallback) {
    u32 est_w = (want->width + 15u) & ~15u;   /* the adapter may widen the scanline */
    u32 need  = est_w * (want->bpp >> 3) * want->height;

    if (desktop_cache) { kfree(desktop_cache); desktop_cache = NULL; }
    desktop_cache_valid = 0;
    if (backbuf) { kfree(backbuf); backbuf = NULL; }
    fb_set_target(NULL);

    u32 *nb = (u32 *)kmalloc(need);
    if (!nb) {
        backbuf = (u32 *)kmalloc(fb_pitch_get() * fb_height());
        fb_set_target(backbuf);
        return VBE_ERR_NOVRAM;
    }

    int r = vbe_set_mode(want->width, want->height, want->bpp);
    if (r != VBE_OK) {
        kfree(nb);
        backbuf = (u32 *)kmalloc(fb_pitch_get() * fb_height());
        fb_set_target(backbuf);
        return r;
    }

    /* the adapter may have taken a wider scanline than estimated */
    if (fb_pitch_get() * fb_height() > need) {
        kfree(nb);
        nb = (u32 *)kmalloc(fb_pitch_get() * fb_height());
        if (!nb) {
            if (fallback) vbe_set_mode(fallback->width, fallback->height, fallback->bpp);
            backbuf = (u32 *)kmalloc(fb_pitch_get() * fb_height());
            fb_set_target(backbuf);
            scr_w = fb_width();
            scr_h = fb_height();
            mouse_set_bounds((i32)scr_w, (i32)scr_h);
            mouse_set_pos((i32)scr_w / 2, (i32)scr_h / 2);
            return VBE_ERR_NOVRAM;
        }
    }

    scr_w = fb_width();
    scr_h = fb_height();
    palette_init();
    mouse_set_bounds((i32)scr_w, (i32)scr_h);
    mouse_set_pos((i32)scr_w / 2, (i32)scr_h / 2);

    backbuf = nb;
    fb_set_target(backbuf);

    /* windows must not be left beyond the edge of the new screen */
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        if (wins[i].x > (i32)scr_w - 80) wins[i].x = (i32)scr_w - 80;
        if (wins[i].y > (i32)scr_h - PANEL_H - TITLE_H)
            wins[i].y = (i32)scr_h - PANEL_H - TITLE_H;
        if (wins[i].x < 0) wins[i].x = 0;
        if (wins[i].y < 0) wins[i].y = 0;
    }
    return VBE_OK;
}

/* Put the previous mode back when the user did not confirm in time. */
static void mode_revert_now(void) {
    revert_armed = 0;
    if (mode_switch_buffered(&revert_mode, NULL) == VBE_OK)
        set_status(T("Resolution reverted - no confirmation",
                     "Разрешение возвращено - нет подтверждения"), C_YELLOW);
}

static void settings_apply(void) {
    int changed = 0;

    if (set_res_sel >= 0) {
        vmode_t m, cur;
        /* a stale chip id must not be read as an uninitialised mode */
        if (!vbe_mode_get((u32)set_res_sel, &m)) { set_res_sel = -1; return; }
        vbe_current(&cur);

        if (m.width != cur.width || m.height != cur.height || m.bpp != cur.bpp) {
            int r = mode_switch_buffered(&m, &cur);
            if (r != VBE_OK) {
                set_status(r == VBE_ERR_NOVRAM
                             ? T("Not enough kernel memory for this resolution",
                                 "Не хватает памяти ядра для этого разрешения")
                             : vbe_error_text(r), C_RED);
                set_res_sel = -1;
                return;
            }

            /* A mode the monitor cannot display leaves the user with a
               blank screen and no way back. Arm an automatic undo: the
               change sticks only if it is confirmed, exactly like every
               desktop OS does. */
            revert_mode     = cur;
            revert_deadline = timer_ticks() + (u64)timer_hz() * REVERT_SECONDS;
            revert_armed    = 1;
            changed = 1;
        }
        set_res_sel = -1;
    }

    if (set_hz_sel) {
        int r = vbe_set_refresh(set_hz_sel);
        if (r == VBE_OK) {
            set_status(T("Mode and refresh rate applied", "Режим и частота применены"), C_GREEN);
            changed = 1;
        } else if (r == VBE_WARN_VIRTUAL) {
            set_status(T("The host system sets the refresh rate", "Частоту задаёт хост-система"), C_YELLOW);
        } else if (r == VBE_ERR_UNSUPPORTED) {
            /* Refusing is deliberate: reprogramming the CRTC under a
               linear framebuffer would desync the monitor. */
            set_status(T("The refresh rate is fixed by the video mode",
                         "Частота задана видеорежимом и не меняется"), C_YELLOW);
        } else {
            set_status(vbe_error_text(r), C_RED);
        }
        set_hz_sel = 0;
        return;
    }

    set_status(changed ? T("Resolution applied", "Разрешение применено") : T("No changes", "Изменений нет"), 
               changed ? C_GREEN : C_TEXT_DIM);
}

/* A click inside the file manager */
static int files_click(int id, i32 mx, i32 my) {
    static u64 last_tick;
    static int last_row=-1,last_pane=-1;
    static u32 last_listing,last_namespace;
    int w = hit_test(id, mx, my);
    if(w<W_FM_ROW0)last_row=last_pane=-1; /* toolbar/background interrupts a double click */
    if (w < 0) return 0;

    if(fm_input_active()) {
        if(w==W_FM_APPLY)fm_input_submit();
        else if(w==W_FM_CANCEL)fm_input_cancel();
        return 1; /* toolbar/panes may not retarget an open name dialog */
    }
    /* the modal question comes first: nothing else may be touched */
    if (fm_confirm_pending()) {
        if (w == W_FM_YES) { fm_confirm_yes(); return 1; }
        if (w == W_FM_NO)  { fm_confirm_no();  return 1; }
        return 1;
    }
    if (fm_view_is_open()) {
        if (w == W_FM_VIEWCLS) { fm_close_view(); return 1; }
        return 1;
    }

    switch (w) {
        case W_FM_UP:      fm_go_up();       return 1;
        case W_FM_VOLUME:  fm_next_volume(); return 1;
        case W_FM_COPY:    fm_do_copy();     return 1;
        case W_FM_MKDIR:   fm_ask_mkdir();   return 1;
        case W_FM_RENAME:  fm_ask_rename();  return 1;
        case W_FM_DEL:     fm_ask_delete();  return 1;
        case W_FM_REFRESH: fm_refresh(); fm_say(T("Refreshed", "Обновлено"), 0); return 1;
        case W_FM_PANE0:   fm_set_active(0); return 1;
        case W_FM_PANE1:   fm_set_active(1); return 1;
    }

    /* a row: select it, and open it on a double click */
    if (w >= W_FM_ROW0) {
        int rel  = w - W_FM_ROW0;
        int pane = rel / 512;
        int row  = rel % 512;

        if(pane>1 || !fm_pane_item(pane,row))return 1;
        u32 listing=fm_listing_epoch(),namespace=vfs_generation();
        fm_set_active(pane);
        fm_pane_set_sel(pane, row);

        u64 now = timer_ticks();
        if (row == last_row && pane == last_pane && listing==last_listing &&
            namespace==last_namespace && now - last_tick < 400)fm_activate();
        /* Capture the pre-activate identity: entering a directory changes it. */
        last_listing=listing;last_namespace=namespace;
        last_tick = now; last_row = row; last_pane = pane;
        return 1;
    }
    return 0;
}

/* A click inside the Settings window */
static int settings_click(int id, i32 mx, i32 my) {
    int w = hit_test(id, mx, my);
    if (w < 0) return 0;

    if (w >= W_TAB && w < W_TAB + 3) { set_tab = w - W_TAB; return 1; }

    if (w >= W_RES && w < W_RES + 64) {
        set_res_sel = w - W_RES;
        set_status(T("Press Apply to change the mode", "Нажмите «Применить» для смены режима"), C_TEXT_DIM);
        return 1;
    }
    if (w >= W_HZ && w < W_HZ + HZ_COUNT) {
        set_hz_sel = hz_list[w - W_HZ];
        set_status(T("Press Apply", "Нажмите «Применить»"), C_TEXT_DIM);
        return 1;
    }
    if (w >= W_ACCENT && w < W_ACCENT + ACCENT_COUNT) {
        theme_accent = w - W_ACCENT;
        settings_theme_accent = theme_accent;
        palette_init();
        settings_save();
        return 1;
    }
    if (w >= W_WALL && w < W_WALL + WALL_COUNT) {
        theme_wall = w - W_WALL;
        settings_theme_wall = theme_wall;
        settings_save();
        return 1;
    }

    switch (w) {
        case W_APPLY:    settings_apply(); return 1;
        case W_SHADOWS:  opt_shadows = !opt_shadows; return 1;
        case W_SECONDS:  opt_seconds = !opt_seconds; return 1;
        case W_AA:       fb_set_aa(!fb_aa_on()); desktop_cache_valid = 0; damage_full(); return 1;
        case W_QUIT:     ed_request_quit(); return 1;
    }
    return 0;
}

#include "gui_editor_input.h"

static void handle_click(i32 mx, i32 my, int pressed) {
    i32 py = (i32)scr_h - PANEL_H;

    if (!pressed) { dragging = ed_drag_win = -1; return; }

    if (mx >= 6 && mx < 84 && my >= py) {
        launcher_open=!launcher_open; damage_full(); return;
    }
    if (launcher_open) {
        i32 y=py-30-(ICON_COUNT+2)*26;
        int row=(my-y-30)/26;
        if(mx>=6 && mx<286 && my>=y+30 && my<py) {
            launcher_open=0; damage_full();
            if(row<ICON_COUNT) open_app(icons[row].app);
            else if(row==ICON_COUNT) kbd_mousekeys_set(!kbd_mousekeys());
            else ed_request_quit();
        } else { launcher_open=0; damage_full(); }
        return;
    }

    /* taskbar */
    if (my >= py) {
        i32 bx = 94;
        for (int i = 0; i < win_count; i++) {
            int id = z_order[i];
            if (!wins[id].used) continue;
            if (mx >= bx && mx < bx + 132) {
                if (id == top_window()) wins[id].minimized = !wins[id].minimized;
                else { wins[id].minimized = 0; raise_window(id); }
                return;
            }
            bx += 138;
        }
        return;
    }

    /* windows from top to bottom */
    for (int i = win_count - 1; i >= 0; i--) {
        int id = z_order[i];
        window_t *v = &wins[id];
        if (!v->used || !v->visible || v->minimized) continue;
        if (mx < v->x || mx >= v->x + v->w || my < v->y || my >= v->y + v->h) continue;

        raise_window(id);

        /* closing */
        if (my >= v->y + 5 && my < v->y + 18 &&
            mx >= v->x + v->w - 20 && mx < v->x + v->w - 6) {
            if(v->app==APP_EDIT || v->app==APP_DEV) {
                editor=v->app==APP_DEV?&dev_editor:&note_editor;
                if(ed_name_mode) { ed_cancel(); return; }
                ed_request(id,ED_CLOSE);
            } else win_close(id);
            return;
        }
        /* dragging by the title bar */
        if (my < v->y + TITLE_H) {
            dragging = id;
            drag_dx = mx - v->x;
            drag_dy = my - v->y;
            return;
        }
        /* the Settings controls */
        if (v->app == APP_SETTINGS) {
            if (settings_click(id, mx, my)) return;
        }

        /* the file manager */
        if (v->app == APP_FILES) {
            if (files_click(id, mx, my)) return;
        }

        /* Programs: formatting the disk, launching and removing */
        if (v->app == APP_STORE) {
            for (int k = 0; k < hit_count; k++) {
                hit_t *ht = &hits[k];
                if (ht->win != id) continue;
                if (mx < ht->x || mx >= ht->x + ht->w) continue;
                if (my < ht->y || my >= ht->y + ht->h) continue;

                if(store_format_confirm) {
                    if(ht->id==W_ST_FORMAT_NO){store_format_confirm=0;store_say(T("Format cancelled", "Форматирование отменено"));}
                    else if(ht->id==W_ST_FORMAT_YES)store_format_apply();
                    return;
                }
                if (ht->id == W_ST_SETUP || ht->id == W_ST_SETUP_KEEP) {
                    /* Installation takes seconds: the message is drawn
                       beforehand, otherwise the screen freezes with no
                       explanation. */
                    store_say(T("Installing... please wait", "Установка... подождите"));
                    int rc = setup_install(ht->id == W_ST_SETUP_KEEP);
                    store_say(rc == 0 ? T("Installed! Remove the media and reboot", "Установлено! Извлеките носитель и перезагрузитесь")
                                      : setup_last_result());
                    if (rc == 0) { beep(880, 60); beep(1320, 90); }
                    return;
                }
                if(ht->id==W_ST_FORMAT) {
                    int d=ata_boot_drive();
                    if(!ata_present() || d<0 || !ata_sectors(d)) {store_say(T("No boot ATA disk", "Нет загрузочного ATA диска"));return;}
                    store_format_request=(store_format_request_t){d,ata_sectors(d),vfs_generation()};
                    store_format_confirm=1;return;
                }
                if(ht->id==W_ST_REFRESH){store_mount_check();return;}
                if (ht->id >= W_ST_RAM0) {
                    rfile_t *tbl = ramfs_table();
                    int ri = ht->id - W_ST_RAM0;
                    if (ri >= 0 && ri < RAMFS_MAX_FILES && tbl[ri].used) {
                        launch_user_app(tbl[ri].name, T("Started from the image", "Запущено из образа"));
                    }
                    return;
                }
                if (ht->id >= W_ST_DEL0) {
                    char nm[44];
                    if (kvfs_list(ht->id - W_ST_DEL0, nm, NULL, NULL) == 0) {
                        /* a running program is unloaded first */
                        if (kapp_loaded() && !strcmp(kapp_filename(), nm)) {
                            kapp_unload();
                            for (int q = 0; q < MAX_WIN; q++)
                                if (wins[q].used && wins[q].app == APP_USER) win_close(q);
                        }
                        int rc = kvfs_delete(nm);
                        store_say(rc == 0 ? T("Program removed", "Программа удалена") : kvfs_error(rc));
                    }
                    return;
                }
                if (ht->id >= W_ST_ITEM0) {
                    char nm[44];
                    if (kvfs_list(ht->id - W_ST_ITEM0, nm, NULL, NULL) == 0) {
                        /* the old application window is closed: only one program at a time */
                        launch_user_app(nm, T("Started", "Запущено"));
                    }
                    return;
                }
            }
            return;
        }

        /* A click inside an application window is handed to the program itself */
        if (v->app == APP_USER && kapp_loaded()) {
            i32 ux = v->x + 1, uy = v->y + TITLE_H + 1;
            i32 uw = v->w - 2, uh = v->h - TITLE_H - 2 - 20;
            if (mx >= ux && mx < ux + uw && my >= uy && my < uy + uh)
                kapp_click(mx - ux, my - uy, 1, ux, uy, uw, uh);
            return;
        }

        if(v->app==APP_EDIT || v->app==APP_DEV) {
            editor=v->app==APP_DEV?&dev_editor:&note_editor;
            ed_click(id,mx,my); return;
        }

        /* drawing inside the canvas */
        if (v->app == APP_PAINT) {
            i32 gx = v->x + 11, gy = v->y + TITLE_H + 27;
            /* colour selection */
            i32 by = v->y + TITLE_H + 5;
            for (int k = 0; k < 6; k++) {
                i32 bx2 = v->x + 181 + k * 22;
                if (mx >= bx2 && mx < bx2 + 18 && my >= by && my < by + 16) {
                    paint_color = (u8)k;
                    return;
                }
            }
            /* Division of negatives in C rounds towards zero: with mx
               1..7 pixels left of the canvas (mx-gx == -1) the result was
               c == 0, a false hit on the first cell. It is rejected
               before the division. */
            i32 dx = mx - gx, dy = my - gy;
            if (dx >= 0 && dy >= 0) {
                i32 c = dx / 8, r = dy / 8;
                if (c < PAINT_W && r < PAINT_H) paint_grid[r][c] = paint_color;
            }
        }
        return;
    }

    /* desktop icons */
    for (int i = 0; i < ICON_COUNT; i++) {
        i32 x = icons[i].x - 6, y = icons[i].y - 6;
        if (mx >= x && mx < x + ICON_W && my >= y && my < y + ICON_H) {
            sel_icon=i;
            open_app(icons[i].app); /* one click, like the launcher */
            return;
        }
    }
    sel_icon = -1;
}

static void handle_drag(i32 mx, i32 my) {
    if (dragging < 0) return;
    window_t *v = &wins[dragging];
    v->x = mx - drag_dx;
    v->y = my - drag_dy;
    if (v->x < -(v->w - 60)) v->x = -(v->w - 60);
    if (v->y < 0) v->y = 0;
    if (v->x > (i32)scr_w - 60) v->x = (i32)scr_w - 60;
    if (v->y > (i32)scr_h - PANEL_H - TITLE_H) v->y = (i32)scr_h - PANEL_H - TITLE_H;
}

static void handle_key(kbd_event_t event) {
    int c=event.key;
    if(c==KEY_LAYOUT) { damage_full(); return; }
    int top = top_window();
    if(top>=0 && wins[top].app==APP_STORE && store_format_confirm) {
        if(c==27){store_format_confirm=0;store_say(T("Format cancelled", "Форматирование отменено"));}
        return; /* Enter never confirms an erase. */
    }

    if(launcher_open) {
        if(c==27) { launcher_open=0; damage_full(); }
        return;
    }

    /* ----- An application on top: keys go to it -----
       Esc is kept for ourselves, otherwise a hung program would be
       impossible to leave. */
    if (top >= 0 && wins[top].app == APP_USER && kapp_loaded() && c != 27) {
        window_t *v = &wins[top];
        kapp_key(c, v->x + 1, v->y + TITLE_H + 1,
                 v->w - 2, v->h - TITLE_H - 2 - 20);
        return;
    }

    if(top>=0 && wins[top].app==APP_USER && c==27) { win_close(top); return; }

    /* ----- The file manager on top: it handles navigation itself.
       Esc dismisses its own dialogs, never the desktop. ----- */
    if (top >= 0 && wins[top].app == APP_FILES) {
        window_t *v = &wins[top];
        int rows = fm_rows_visible(v);
        if (c == 27 && !fm_view_is_open() && !fm_confirm_pending() && !fm_input_active()) {
            /* No editor/viewer dialog: Escape leaves the window open. */
        } else if(fm_input_active() && event.codepoint>=128) {
            fm_input_codepoint(event.codepoint);return;
        } else if (fm_key(c, rows)) {
            return;
        }
    }

    if(top>=0 && (wins[top].app==APP_EDIT || wins[top].app==APP_DEV)) {
        editor=wins[top].app==APP_DEV?&dev_editor:&note_editor;
        ed_key(top,c,event.codepoint,event.modifiers,event.physical); return;
    }

    /* when the terminal is open and on top, input goes there */
    if (top >= 0 && wins[top].app == APP_TERM) {
        if (c == '\n') {
            term_input[term_len] = 0;
            term_exec(term_input);
            term_len = 0;
            term_input[0] = 0;
            return;
        }
        if (c == '\b') {
            if (term_len) term_input[--term_len] = 0;
            return;
        }
        if (c == 27) return;
        if (c >= 32 && c < 127 && term_len < TERM_COLS - 10) {
            term_input[term_len++] = (char)c;
            term_input[term_len] = 0;
            return;
        }
        return;
    }

    /* No letter-based desktop shortcuts. Text belongs to the focused app;
       launchers, close buttons and the panel provide mouse navigation. */
}

typedef struct {
    window_t wins[MAX_WIN];
    int z_order[MAX_WIN];
    int win_count, sel_icon;
    int theme_accent, theme_wall, opt_shadows, opt_seconds;
    int revert_armed;
    kv_lang_t language;
    u32 scr_w, scr_h, pitch;
    u8 bpp;
} gui_snapshot_t;

static void gui_snapshot_take(gui_snapshot_t *s) {
    memcpy(s->wins, wins, sizeof(wins));
    memcpy(s->z_order, z_order, sizeof(z_order));
    s->win_count = win_count;
    s->sel_icon = sel_icon;
    s->theme_accent = theme_accent;
    s->theme_wall = theme_wall;
    s->opt_shadows = opt_shadows;
    s->opt_seconds = opt_seconds;
    s->revert_armed = revert_armed;
    s->language = kv_lang_get();
    s->scr_w = scr_w; s->scr_h = scr_h;
    s->pitch = fb_pitch_get();
    s->bpp = fb_bpp_get();
}

static int snapshot_window_visible(const window_t *v) {
    return v->used && v->visible && !v->minimized;
}

static int snapshot_order(const int *order, int count, int id) {
    for (int i = 0; i < count; i++) if (order[i] == id) return i;
    return -1;
}

static int window_visual_state_changed(const window_t *a, const window_t *b) {
    return a->used != b->used || a->visible != b->visible ||
           a->minimized != b->minimized || a->x != b->x || a->y != b->y ||
           a->w != b->w || a->h != b->h || a->app != b->app ||
           a->accent != b->accent || strcmp(a->title, b->title) != 0;
}

static void damage_snapshot_window(const window_t *v) {
    if (snapshot_window_visible(v)) damage_window_rect(v->x, v->y, v->w, v->h);
}

static void damage_icon(int i) {
    if (i < 0 || i >= ICON_COUNT) return;
    damage_add_rect(icons[i].x - 6, icons[i].y - 6, ICON_W, ICON_H);
}

static int window_at(i32 x, i32 y) {
    for (int i = win_count - 1; i >= 0; i--) {
        int id = z_order[i];
        window_t *v = &wins[id];
        if (v->used && v->visible && !v->minimized &&
            x >= v->x && x < v->x + v->w && y >= v->y && y < v->y + v->h)
            return id;
    }
    return -1;
}

static void gui_damage_after_input(const gui_snapshot_t *before, int content_win) {
    int geometry_changed = before->scr_w != scr_w || before->scr_h != scr_h ||
                           before->pitch != fb_pitch_get() || before->bpp != fb_bpp_get();
    if (geometry_changed) {
        desktop_cache_valid = 0;
        layout_icons();
        damage_full();
        return;
    }

    if (before->theme_accent != theme_accent || before->theme_wall != theme_wall ||
        before->language != kv_lang_get()) {
        desktop_cache_valid = 0;
        damage_full();
        return;
    }

    if (before->sel_icon != sel_icon) {
        /* Selection is part of the cached desktop layer, but only its old
           and new icon bounds need to be presented after rebuilding it. */
        desktop_cache_valid = 0;
        damage_icon(before->sel_icon);
        damage_icon(sel_icon);
    }

    if (before->opt_shadows != opt_shadows) {
        for (int i = 0; i < MAX_WIN; i++) {
            damage_snapshot_window(&before->wins[i]);
            damage_window(&wins[i]);
        }
    }

    int panel_changed = before->win_count != win_count ||
                        before->opt_seconds != opt_seconds ||
                        before->revert_armed != revert_armed;
    if (before->revert_armed != revert_armed)
        damage_add_rect(0, 0, (i32)scr_w, 48); /* confirmation banner */

    for (int i = 0; i < MAX_WIN; i++) {
        const window_t *old = &before->wins[i];
        const window_t *now = &wins[i];
        int order_changed = snapshot_order(before->z_order, before->win_count, i) !=
                            snapshot_order(z_order, win_count, i);
        if (window_visual_state_changed(old, now) || order_changed) {
            damage_snapshot_window(old);
            damage_window(now);
            panel_changed = 1;
        }
    }

    /* App callbacks and built-in controls can mutate private state that is
       not represented in window_t; conservatively repaint that one window. */
    if (content_win >= 0 && content_win < MAX_WIN) {
        damage_snapshot_window(&before->wins[content_win]);
        damage_window(&wins[content_win]);
    }
    if (panel_changed) damage_panel();
}

/* ---------- main loop ---------- */

int gui_active(void) { return running; }

int gui_run(void) {
    if (!fb_active()) return -1;

    scr_w = fb_width();
    scr_h = fb_height();
    if (settings_theme_accent >= 0 && settings_theme_accent < ACCENT_COUNT)
        theme_accent = settings_theme_accent;
    if (settings_theme_wall >= 0 && settings_theme_wall < WALL_COUNT)
        theme_wall = settings_theme_wall;
    palette_init();

    u32 bytes = fb_pitch_get() * scr_h;
    backbuf = (u32 *)kmalloc(bytes);
    /* Without a backbuffer every primitive goes straight to uncached
       video memory - on real hardware that is unusably slow and looks
       like a freeze. Better to refuse the desktop than to hang it. */
    if (!backbuf) return -2;
    fb_set_target(backbuf);

    memset(wins, 0, sizeof(wins));
    win_count = 0; dragging = ed_drag_win = -1; sel_icon = -1;
    launcher_open=0; ed_quit_mask=0;
    memset(paint_grid, 0, sizeof(paint_grid));
    term_row = 0; term_len = 0; term_input[0] = 0;
    memset(term_buf, 0, sizeof(term_buf));
    term_putline(T("KvantGUI terminal. Type help.", "KvantGUI терминал. Наберите help."));

    mouse_set_bounds((i32)scr_w, (i32)scr_h);
    mouse_set_pos((i32)scr_w / 2, (i32)scr_h / 2);

    /* The first complete frame also builds the static desktop cache. After
       that, each event contributes only the rectangles whose pixels changed. */
    layout_icons();
    damage_full();
    u32 fps_frames = 0;
    u64 fps_mark = timer_ticks();
    i32 prev_mx = mouse_x(), prev_my = mouse_y(); /* last rendered cursor */
    i32 seen_mx = prev_mx, seen_my = prev_my;     /* last input sample */
    u32 refresh_period = gui_refresh_period();
    u64 next_refresh = timer_ticks() + refresh_period;
    int was_animated = 0;

    open_app(APP_ABOUT);

    gui_mouse_seen=0;
    running = 1;
    int prev_btn = 0;

    while (running) {
        /* keyboard input */
        kbd_event_t event;
        while(kbd_getevent_nb(&event)) {
            int c=event.key;
            gui_snapshot_t before;
            gui_snapshot_take(&before);

            /* Any deliberate keystroke proves the screen is readable:
               Enter/Y confirms the new mode, Esc undoes it at once. */
            if (revert_armed) {
                if (c == 27 || c == 'n' || c == 'N') {
                    mode_revert_now();
                    gui_damage_after_input(&before, -1);
                    continue;
                }
                revert_armed = 0;
                set_status(T("Resolution confirmed", "Разрешение подтверждено"), C_GREEN);
                if (c == '\n' || c == '\r' || c == 'y' || c == 'Y') {
                    gui_damage_after_input(&before, -1);
                    continue;
                }
            }

            int content_win = top_window();
            handle_key(event);
            gui_damage_after_input(&before, content_win);
        }

        /* the user never confirmed - the screen is probably blank */
        if (revert_armed && timer_ticks() >= revert_deadline) {
            gui_snapshot_t before;
            gui_snapshot_take(&before);
            mode_revert_now();
            gui_damage_after_input(&before, -1);
        }

        /* mouse */
        i32 mx = mouse_x(), my = mouse_y();
        int btn = mouse_buttons() & 1;
        int mouse_moved = (mx != seen_mx || my != seen_my);
        if (btn && !prev_btn) {
            gui_snapshot_t before;
            gui_snapshot_take(&before);
            int content_win = window_at(mx, my);
            if (revert_armed) {
                revert_armed = 0;
                set_status(T("Resolution confirmed", "Разрешение подтверждено"), C_GREEN);
            }
            handle_click(mx, my, 1);
            gui_damage_after_input(&before, content_win);
        }
        else if (!btn && prev_btn) {
            handle_click(mx, my, 0);
        }
        else if (btn && prev_btn && mouse_moved) {
            if (dragging >= 0 && wins[dragging].used) {
                gui_snapshot_t before;
                gui_snapshot_take(&before);
                int content_win = dragging;
                handle_drag(mx, my);
                gui_damage_after_input(&before, content_win);
            } else {
                int top = top_window();
                if (top >= 0 && wins[top].app == APP_PAINT) {
                    gui_snapshot_t before;
                    gui_snapshot_take(&before);
                    handle_click(mx, my, 1);  /* paint only on a new mouse sample */
                    gui_damage_after_input(&before, top);
                }
            }
        }
        if(btn && prev_btn && ed_drag_win>=0) ed_drag(mx,my);
        int wheel=mouse_wheel_delta();
        if(wheel && !launcher_open) ed_wheel(mx,my,wheel);
        prev_btn = btn;
        gui_mouse_seen=btn;
        seen_mx = mx; seen_my = my;

        /* Apps receive one elapsed-time update per rendered frame, while
           rendering and transfer stay confined to their damaged window. */
        u64 frame_start = timer_ticks();
        u64 loop_now = frame_start;
        int animated_id = gui_animated_window();
        int animate = animated_id >= 0;
        if (animate != was_animated) {
            fps_frames = 0;
            fps_mark = loop_now;
            gui_fps = 0;
            was_animated = animate;
            damage_panel();
        }
        u32 wanted_period = gui_refresh_period();
        if (wanted_period != refresh_period) {
            refresh_period = wanted_period;
            next_refresh = loop_now + refresh_period;
        }

        if (animate) {
            window_t *v = &wins[animated_id];
            damage_window(v);
            damage_panel(); /* the FPS readout changes with animation */
            if (revert_armed) damage_add_rect(0, 0, (i32)scr_w, 48);
        } else if (loop_now >= next_refresh) {
            /* Live widgets and expiring status text need a periodic update,
               but only their own windows are damaged (not the wallpaper). */
            for (int i = 0; i < MAX_WIN; i++)
                if (wins[i].used && wins[i].visible && !wins[i].minimized)
                    damage_window(&wins[i]);
            damage_panel();
            if (revert_armed) damage_add_rect(0, 0, (i32)scr_w, 48);
            next_refresh = loop_now + refresh_period;
        }

        /* Launcher row hover highlight follows the pointer */
        if (launcher_open && mouse_moved) {
            i32 py = (i32)scr_h - PANEL_H, ly = py - 30 - (ICON_COUNT + 2) * 26;
            damage_add_rect(6, ly, 280, py - ly);
        }

        /* Icon hover: pill + tooltip. The cached desktop is rebuilt only
           when the hover target actually changes. */
        {
            int hov = (!launcher_open && dragging < 0 &&
                       window_at(mx, my) < 0) ? icon_at(mx, my) : -1;
            if (hov != hover_icon) {
                damage_icon(hover_icon);
                damage_icon(hov);
                damage_tooltip(hover_icon);
                damage_tooltip(hov);
                hover_icon = hov;
            }
        }

        /* Restore the old pointer and paint the new one from the clean scene. */
        if (mx != prev_mx || my != prev_my) {
            damage_add_rect(prev_mx, prev_my, 13, 17);
            damage_add_rect(mx, my, 13, 17);
        }

        /* An idle desktop sleeps until an IRQ. Animation uses the selected
           frame cap; its on_tick delta remains based on elapsed wall time. */
        if (!dirty_count && !dirty_all) {
            __asm__ volatile("hlt" ::: "memory");
            continue;
        }

        render_damage();

        /* Keep the cursor out of both buffers. Any later damage under it is
           reconstructed from the desktop cache and window layers. */
        fb_set_target(NULL);
        draw_cursor(mx, my);
        fb_set_target(backbuf);
        prev_mx = mx; prev_my = my;

        /* Count animation frames only; sparse idle updates are not FPS data. */
        if (animate) {
            fps_frames++;
            u64 fps_now = timer_ticks();
            u32 hz = timer_hz();
            if (fps_now - fps_mark >= hz) {
                u32 span = (u32)(fps_now - fps_mark);
                gui_fps = span ? (fps_frames * hz / span) : 0;
                fps_frames = 0;
                fps_mark = fps_now;
            }
        }

        /* Animated applications are capped at 60 fps by default. The
           scheduler still gets control between frames. */
        if (gui_fps_limit) {
            u32 hz     = timer_hz();
            u32 budget = hz / gui_fps_limit;                   /* ticks per frame */
            u32 spent  = (u32)(timer_ticks() - frame_start);
            if (spent < budget) {
                u32 left_ms = (budget - spent) * 1000u / hz;
                if (left_ms) task_sleep(left_ms);
                else         task_yield();
            } else {
                task_yield();
            }
        } else {
            task_yield();      /* no limit - just yield the CPU */
        }
    }

    store_format_confirm=0;
    if(fm_input_active())fm_input_cancel();
    kapp_unload(); /* releases isolated pages/grants even on GUI exit */
    fb_clear_clip();
    fb_set_target(NULL);
    if (desktop_cache) { kfree(desktop_cache); desktop_cache = NULL; }
    desktop_cache_valid = 0;
    if (backbuf) { kfree(backbuf); backbuf = NULL; }
    return 0;
}
