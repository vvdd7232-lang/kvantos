/* ============================================================
 *  KvantOS - the graphical shell (KvantGUI)
 *  Desktop, windows, taskbar, mouse, mini-applications.
 *  Drawing goes into a back buffer that is then blitted to screen.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"
#include "filemgr.h"

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
static u32     *backbuf = NULL;

/* Pending confirmation of a resolution change (the 15-second undo). */
static int      revert_armed = 0;
static vmode_t  revert_mode;
static u64      revert_deadline = 0;
static u32      scr_w, scr_h;

/* palette */
static u32 C_DESK1, C_DESK2, C_PANEL, C_PANEL2, C_WIN, C_WINBORDER;
static u32 C_TITLE, C_TITLE_IN, C_TEXT, C_TEXT_DIM, C_WHITE, C_BLACK;
static u32 C_ACCENT, C_GREEN, C_RED, C_YELLOW, C_CYAN, C_SHADOW;

enum { APP_ABOUT = 0, APP_SYSMON, APP_FILES, APP_TERM, APP_PAINT, APP_HELP, APP_SETTINGS, APP_EDIT,
       APP_STORE,      /* installing and launching programs */
       APP_USER };     /* the window of a running .kapp application */

/* Application names. A static initialiser cannot contain a function
   call, so both languages live in separate tables and app_name()
   returns the right string. */
static const char *app_names_en[] = {
    "About", "Monitor", "Files", "Terminal", "Paint", "Help", "Settings", "Notepad",
    "Programs", "Application"
};
static const char *app_names_ru[] = {
    "О системе", "Монитор", "Файлы", "Терминал", "Рисование", "Справка", "Настройки", "Блокнот",
    "Программы", "Приложение"
};
static const char *app_name(int i)
{
    return kv_pick(app_names_en[i], app_names_ru[i]);
}

/* ---------- appearance settings, changed in Settings ---------- */
static int  theme_accent = 0;      /* accent colour index */
static int  theme_wall   = 0;      /* desktop background style */
static int  opt_seconds  = 1;      /* show seconds in the clock */
static int  opt_shadows  = 1;      /* shadows under windows */

static const struct { u8 r, g, b; } accents[] = {
    {  38,  92, 168 },
    {  20, 130, 140 },
    {  32, 132,  84 },
    { 118,  62, 156 },
    { 166,  54,  70 },
    {  70,  78,  96 },
};
static const char *accent_names_en[] = {
    "Blue", "Teal", "Emerald", "Purple", "Garnet", "Graphite"
};
static const char *accent_names_ru[] = {
    "Синий", "Бирюзовый", "Изумруд", "Пурпур", "Гранат", "Графит"
};
static const char *accent_name(int i)
{
    return kv_pick(accent_names_en[i], accent_names_ru[i]);
}
#define ACCENT_COUNT ((int)(sizeof(accents) / sizeof(accents[0])))

static const char *wall_names_en[] = { "Gradient", "Night", "Grid", "Solid" };
static const char *wall_names_ru[] = { "Градиент", "Ночь", "Сетка", "Однотонный" };
static const char *wall_name(int i)
{
    return kv_pick(wall_names_en[i], wall_names_ru[i]);
}
#define WALL_COUNT 4

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

/* ---------- housekeeping ---------- */

static void palette_init(void) {
    C_DESK1     = fb_rgb(24, 38, 66);
    C_DESK2     = fb_rgb(12, 18, 34);
    C_TITLE     = fb_rgb(accents[theme_accent].r,
                         accents[theme_accent].g,
                         accents[theme_accent].b);
    C_PANEL     = fb_rgb(20, 26, 42);
    C_PANEL2    = fb_rgb(34, 44, 68);
    C_WIN       = fb_rgb(238, 240, 245);
    C_WINBORDER = fb_rgb(90, 100, 125);
    C_TITLE_IN  = fb_rgb(96, 104, 122);
    C_TEXT      = fb_rgb(22, 26, 34);
    C_TEXT_DIM  = fb_rgb(110, 118, 132);
    C_WHITE     = fb_rgb(255, 255, 255);
    C_BLACK     = fb_rgb(0, 0, 0);
    C_ACCENT    = fb_rgb(64, 156, 255);
    C_GREEN     = fb_rgb(72, 200, 120);
    C_RED       = fb_rgb(226, 78, 78);
    C_YELLOW    = fb_rgb(240, 190, 70);
    C_CYAN      = fb_rgb(80, 220, 220);
    C_SHADOW    = fb_rgb(8, 12, 22);
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
    if (id < 0 || !wins[id].used) return;
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
    fb_fill(x, y, w, 12, fb_rgb(210, 214, 222));
    fb_fill(x, y, (i32)((u32)w * pct / 100u), 12, col);
    fb_rect(x, y, w, 12, fb_rgb(170, 176, 188));
}

static void app_about(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 y = cy + 10;
    fb_text(cx + 12, y, "KvantOS " KV_VERSION, C_TEXT, 0xFFFFFFFF); y += 22;
    fb_fill(cx + 12, y, cw - 24, 1, fb_rgb(200, 206, 216)); y += 10;

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

#define ED_ROWS     128        /* lines in a document */
#define ED_COLS     120        /* characters per line */
#define ED_VIEW     20         /* visible lines without scrolling */

static char ed_buf[ED_ROWS][ED_COLS + 1];
static int  ed_lines   = 1;    /* how many lines are used */
static int  ed_cy      = 0;    /* cursor line */
static int  ed_cx      = 0;    /* cursor column (in bytes) */
static int  ed_top     = 0;    /* first visible line */
static int  ed_dirty   = 0;    /* there are unsaved changes */
static char ed_name[24] = "untitled.txt";   /* set in gui_run() when the language changes */
static char ed_status[64] = "";
static u64  ed_status_until = 0;
static int  ed_name_mode = 0;  /* 1 - a file name is being typed */
static char ed_name_in[24] = "";
static int  ed_blink = 0;

/* identifiers of the editor toolbar buttons */
enum { W_ED_NEW = 40, W_ED_OPEN, W_ED_SAVE, W_ED_DEL, W_ED_FILE0 };
/* Programs window: 200 - format the disk, 201 - refresh, 210+ - a list
   row, and 300+ - the Delete button next to that same row. */
enum { W_ST_FORMAT = 200, W_ST_REFRESH = 201, W_ST_ITEM0 = 210, W_ST_DEL0 = 300,
       W_ST_RAM0 = 400,   /* applications from ramfs (the boot image) */
       W_ST_SETUP = 500, W_ST_SETUP_KEEP = 501 };   /* installation onto the hard disk */

static void ed_msg(const char *m) {
    u32 i = 0;
    for (; m[i] && i < sizeof(ed_status) - 1; i++) ed_status[i] = m[i];
    ed_status[i] = 0;
    ed_status_until = timer_ticks() + timer_hz() * 3;
}

static void ed_reset(void) {
    for (int i = 0; i < ED_ROWS; i++) ed_buf[i][0] = 0;
    ed_lines = 1; ed_cy = 0; ed_cx = 0; ed_top = 0; ed_dirty = 0;
}

/* Load a file from ramfs into the line buffer. */
static int ed_load(const char *name) {
    rfile_t *f = ramfs_find(name);
    if (!f) return 0;

    ed_reset();
    int row = 0, col = 0;
    for (u32 i = 0; i < f->size && row < ED_ROWS; i++) {
        char c = f->data[i];
        if (c == '\n') {
            ed_buf[row][col] = 0;
            row++; col = 0;
            continue;
        }
        if (c == '\r') continue;
        if (col < ED_COLS) ed_buf[row][col++] = c;
    }
    if (row < ED_ROWS) ed_buf[row][col] = 0;
    ed_lines = row + 1;
    if (ed_lines > ED_ROWS) ed_lines = ED_ROWS;

    u32 k = 0;
    for (; name[k] && k < sizeof(ed_name) - 1; k++) ed_name[k] = name[k];
    ed_name[k] = 0;
    ed_dirty = 0;
    return 1;
}

/* Save the buffer into ramfs in one piece. */
static int ed_save(void) {
    static char out[ED_ROWS * (ED_COLS + 1)];
    u32 n = 0;
    for (int r = 0; r < ed_lines; r++) {
        for (int c = 0; ed_buf[r][c] && n < sizeof(out) - 2; c++)
            out[n++] = ed_buf[r][c];
        if (r < ed_lines - 1 && n < sizeof(out) - 1) out[n++] = '\n';
    }
    out[n] = 0;

    ramfs_delete(ed_name);            /* rewriting: the old copy is removed */
    int rc = ramfs_create(ed_name, out, n);
    if (rc >= 0) { ed_dirty = 0; ed_msg(T("File saved", "Файл сохранён")); return 1; }
    if (rc == -5) ed_msg(T("Name too long (max 23)", "Имя слишком длинное (макс. 23)"));
    else if (rc == -4) ed_msg(T("Empty file name", "Пустое имя файла"));
    else if (rc == -1) ed_msg(T("No space left in ramfs", "Нет места в ramfs"));
    else ed_msg(T("Save failed", "Ошибка сохранения"));
    return 0;
}

/* Insert a character at the cursor, shifting the rest of the line. */
static void ed_insert_char(char c) {
    char *line = ed_buf[ed_cy];
    int len = 0; while (line[len]) len++;
    if (len >= ED_COLS) { ed_msg(T("Line is full", "Строка заполнена")); return; }
    for (int i = len; i >= ed_cx; i--) line[i + 1] = line[i];
    line[ed_cx] = c;
    ed_cx++;
    ed_dirty = 1;
}

/* Enter: split the line and push the rest down. */
static void ed_newline(void) {
    if (ed_lines >= ED_ROWS) { ed_msg(T("Line limit reached", "Достигнут предел строк")); return; }
    for (int r = ed_lines; r > ed_cy + 1; r--) {
        int k = 0;
        for (; ed_buf[r - 1][k]; k++) ed_buf[r][k] = ed_buf[r - 1][k];
        ed_buf[r][k] = 0;
    }
    char *cur = ed_buf[ed_cy];
    char *nxt = ed_buf[ed_cy + 1];
    int k = 0;
    for (; cur[ed_cx + k]; k++) nxt[k] = cur[ed_cx + k];
    nxt[k] = 0;
    cur[ed_cx] = 0;
    ed_lines++;
    ed_cy++; ed_cx = 0;
    ed_dirty = 1;
}

/* Backspace: either delete a character or join the line with the previous one. */
static void ed_backspace(void) {
    if (ed_cx > 0) {
        char *line = ed_buf[ed_cy];
        for (int i = ed_cx - 1; line[i]; i++) line[i] = line[i + 1];
        ed_cx--;
        ed_dirty = 1;
        return;
    }
    if (ed_cy == 0) return;

    char *prev = ed_buf[ed_cy - 1];
    int plen = 0; while (prev[plen]) plen++;
    char *cur = ed_buf[ed_cy];
    int k = 0;
    for (; cur[k] && plen + k < ED_COLS; k++) prev[plen + k] = cur[k];
    prev[plen + k] = 0;

    for (int r = ed_cy; r < ed_lines - 1; r++) {
        int j = 0;
        for (; ed_buf[r + 1][j]; j++) ed_buf[r][j] = ed_buf[r + 1][j];
        ed_buf[r][j] = 0;
    }
    ed_buf[ed_lines - 1][0] = 0;
    ed_lines--;
    ed_cy--; ed_cx = plen;
    ed_dirty = 1;
}

static void ed_scroll_to_cursor(void) {
    if (ed_cy < ed_top) ed_top = ed_cy;
    if (ed_cy >= ed_top + ED_VIEW) ed_top = ed_cy - ED_VIEW + 1;
    if (ed_top < 0) ed_top = 0;
}

/* forward declarations: the manager is drawn before the widget helpers
   are defined further down this file */
static void widget_chip(int win, int id, i32 x, i32 y, i32 w, i32 h,
                        const char *label, int active);

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
    fb_fill(px, py, pw, ph, C_WHITE);
    fb_rect(px, py, pw, ph, active ? C_ACCENT : fb_rgb(198, 203, 214));
    if (active) fb_rect(px + 1, py + 1, pw - 2, ph - 2, C_ACCENT);

    /* the volume header: name, type, free space */
    const char *path = fm_pane_path(idx);
    u32 tk = 0, fk = 0;
    fm_pane_space(idx, &tk, &fk);      /* cached: see filemgr.c */

    fb_fill(px + 1, py + 1, pw - 2, 20, active ? C_ACCENT : fb_rgb(232, 235, 242));
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
        fb_text(px + 10, y + 6, T("(empty)", "(пусто)"), C_TEXT_DIM, 0xFFFFFFFF);
    }

    for (int r = 0; r < rows; r++) {
        int i = scroll + r;
        if (i >= count) break;
        vfs_dirent_t *e = fm_pane_item(idx, i);
        if (!e) break;

        int is_sel = (i == sel);
        u32 bg = is_sel ? (active ? C_ACCENT : fb_rgb(214, 219, 230))
                        : ((r & 1) ? fb_rgb(247, 248, 251) : C_WHITE);
        fb_fill(px + 2, y, pw - 4, 18, bg);

        u32 fg = is_sel && active ? C_WHITE : C_TEXT;

        /* a small icon: folder or sheet */
        if (e->is_dir) {
            fb_fill(px + 7, y + 4, 6, 2, is_sel && active ? C_WHITE : C_YELLOW);
            fb_fill(px + 7, y + 6, 13, 9, is_sel && active ? C_WHITE : C_YELLOW);
        } else {
            u32 ic = is_sel && active ? C_WHITE : fb_rgb(150, 158, 175);
            fb_fill(px + 8, y + 3, 10, 12, is_sel && active ? C_ACCENT : C_WHITE);
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
        fb_fill(px + pw - 5, track_y, 3, track_h, fb_rgb(228, 231, 238));
        i32 bar = track_h * rows / count;
        if (bar < 12) bar = 12;
        i32 bpos = (count - rows) ? (track_h - bar) * scroll / (count - rows) : 0;
        fb_fill(px + pw - 5, track_y + bpos, 3, bar, fb_rgb(168, 176, 194));
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
    fb_fill(x, y, w, h, C_WHITE);
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
    fb_fill(x + 1, y + h - 20, w - 2, 19, fb_rgb(240, 242, 247));
    fb_text(x + 8, y + h - 18, foot, C_TEXT_DIM, 0xFFFFFFFF);
    (void)v;
}

/* a modal question */
static void fm_draw_confirm(int win, i32 cx, i32 cy, i32 cw, i32 ch) {
    i32 w = 380, h = 120;
    if (w > cw - 20) w = cw - 20;
    i32 x = cx + (cw - w) / 2, y = cy + (ch - h) / 2;

    fb_fill(x + 4, y + 4, w, h, fb_rgb(0, 0, 0));
    fb_fill(x, y, w, h, C_WHITE);
    fb_rect(x, y, w, h, fb_rgb(204, 51, 68));
    fb_fill(x, y, w, 22, fb_rgb(204, 51, 68));
    fb_text_center(x, y + 3, w, T("Confirm deletion", "Подтвердите удаление"), C_WHITE, 0xFFFFFFFF);

    char nm[80];
    fit_name(nm, sizeof(nm), fm_confirm_target(), (u32)((w - 30) / 8));
    fb_text_center(x, y + 38, w, nm, C_TEXT, 0xFFFFFFFF);
    fb_text_center(x, y + 56, w, T("This cannot be undone.", "Отменить это будет нельзя."),
                   C_TEXT_DIM, 0xFFFFFFFF);

    widget_chip(win, W_FM_YES, x + w / 2 - 110, y + h - 34, 100, 26, T("Delete", "Удалить"), 1);
    widget_chip(win, W_FM_NO,  x + w / 2 + 10,  y + h - 34, 100, 26, T("Cancel", "Отмена"), 0);
}

static void app_files(window_t *v, i32 cx, i32 cy, i32 cw) {
    int win = (int)(v - wins);
    i32 ch = v->h - TITLE_H - 2;

    fm_init();

    /* the toolbar */
    i32 tx = cx + 8, ty = cy + 6;
    widget_chip(win, W_FM_UP,      tx,       ty, 46, 24, T("Up", "Вверх"), 0);
    widget_chip(win, W_FM_VOLUME,  tx + 52,  ty, 74, 24, T("Volume", "Том"), 0);
    widget_chip(win, W_FM_COPY,    tx + 132, ty, 92, 24, T("Copy F5", "Копир. F5"), 0);
    widget_chip(win, W_FM_MKDIR,   tx + 230, ty, 92, 24, T("Folder F7", "Каталог F7"), 0);
    widget_chip(win, W_FM_DEL,     tx + 328, ty, 92, 24, T("Delete F8", "Удалить F8"), 0);
    widget_chip(win, W_FM_REFRESH, tx + 426, ty, 76, 24, T("Refresh", "Обновить"), 0);

    /* the two panes */
    i32 top = cy + 36;
    i32 body_h = ch - 36 - 26;
    i32 gap = 8;
    i32 pw = (cw - 16 - gap) / 2;

    fm_draw_pane(v, win, 0, cx + 8,           top, pw, body_h);
    fm_draw_pane(v, win, 1, cx + 8 + pw + gap, top, pw, body_h);

    /* the status line */
    i32 sy = cy + ch - 22;
    fb_fill(cx + 1, sy, cw - 2, 21, fb_rgb(240, 242, 247));

    if (fm_input_active()) {
        char line[160];
        ksnprintf(line, sizeof(line), T("New folder name: %s_", "Имя нового каталога: %s_"),
                  fm_input_text());
        fb_text(cx + 10, sy + 3, line, C_ACCENT, 0xFFFFFFFF);
    } else {
        u32 col = fm_status_colour();
        fb_text(cx + 10, sy + 3, fm_status_text(), col ? col : C_TEXT_DIM, 0xFFFFFFFF);
    }

    if (fm_view_is_open())      fm_draw_view(v, win, cx, cy, cw, ch);
    if (fm_confirm_pending())   fm_draw_confirm(win, cx, cy, cw, ch);
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
    fb_fill(cx + 6, cy + 6, cw - 12, ch - 6, fb_rgb(18, 22, 30));
    i32 y = cy + 12;
    for (int i = 0; i < term_row && i < TERM_ROWS; i++) {
        fb_text(cx + 12, y, term_buf[i],
                term_buf[i][0] == 'k' ? C_CYAN : fb_rgb(200, 210, 220), 0xFFFFFFFF);
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
        fb_rect(bx, by, 18, 16, i == paint_color ? C_BLACK : fb_rgb(180, 186, 196));
    }
    i32 gx = cx + 10, gy = cy + 26;
    fb_fill(gx - 2, gy - 2, PAINT_W * 8 + 4, PAINT_H * 8 + 4, fb_rgb(150, 158, 175));
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
    u32 bg = active ? C_TITLE : fb_rgb(226, 229, 236);
    u32 fg = active ? C_WHITE : C_TEXT;
    fb_round_fill(x, y, w, h, bg);
    if (!active) fb_rect(x, y, w, h, fb_rgb(196, 201, 212));
    fb_text_center(x, y + (h - 16) / 2 + 1, w, label, fg, 0xFFFFFFFF);
    hit_add(win, id, x, y, w, h);
}

/* An on/off checkbox */
static void widget_check(int win, int id, i32 x, i32 y,
                         const char *label, int on) {
    fb_fill(x, y, 16, 16, on ? C_TITLE : C_WHITE);
    fb_rect(x, y, 16, 16, on ? C_TITLE : fb_rgb(170, 176, 190));
    if (on) {                       /* the tick */
        for (int i = 0; i < 4; i++) fb_pixel((u32)(x + 4 + i), (u32)(y + 8 + i), C_WHITE);
        for (int i = 0; i < 6; i++) fb_pixel((u32)(x + 7 + i), (u32)(y + 11 - i), C_WHITE);
    }
    fb_text(x + 24, y, label, C_TEXT, 0xFFFFFFFF);
    hit_add(win, id, x, y, 220, 18);
}

static void section(i32 x, i32 y, i32 w, const char *title) {
    fb_text(x, y, title, C_TEXT, 0xFFFFFFFF);
    fb_fill(x, y + 18, w, 1, fb_rgb(206, 211, 220));
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
    fb_fill(cx + 10, ty + 30, cw - 20, 1, fb_rgb(206, 211, 220));

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
static void app_edit(window_t *v, i32 cx, i32 cy, i32 cw) {
    int wid = (int)(v - wins);
    i32 y = cy + 8;

    /* ----- toolbar ----- */
    struct { int id; const char *cap; i32 w; } btns[] = {
        { W_ED_NEW,  T("New", "Создать"),   78 },
        { W_ED_OPEN, T("Open", "Открыть"),   78 },
        { W_ED_SAVE, T("Save", "Сохранить"), 92 },
        { W_ED_DEL,  T("Delete", "Удалить"),   78 },
    };
    i32 bx = cx + 10;
    for (u32 i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
        fb_round_fill(bx, y, btns[i].w, 24, fb_rgb(238, 241, 246));
        fb_rect(bx, y, btns[i].w, 24, fb_rgb(198, 204, 214));
        fb_text_center(bx, y + 5, btns[i].w, btns[i].cap, C_TEXT, 0xFFFFFFFF);
        hit_add(wid, btns[i].id, bx, y, btns[i].w, 24);
        bx += btns[i].w + 6;
    }
    y += 30;

    /* ----- file name row ----- */
    fb_fill(cx + 10, y, cw - 20, 22, C_WHITE);
    fb_rect(cx + 10, y, cw - 20, 22, ed_name_mode ? C_ACCENT : fb_rgb(200, 206, 216));
    if (ed_name_mode) {
        char tmp[40];
        ksnprintf(tmp, sizeof(tmp), T("Name: %s%s", "Имя: %s%s"), ed_name_in, ed_blink < 15 ? "_" : "");
        fb_text(cx + 16, y + 4, tmp, C_TEXT, 0xFFFFFFFF);
    } else {
        char tmp[48];
        ksnprintf(tmp, sizeof(tmp), "%s%s", ed_name, ed_dirty ? T("  *modified", "  *изменён") : "");
        fb_text(cx + 16, y + 4, tmp, ed_dirty ? C_RED : C_TEXT, 0xFFFFFFFF);
    }
    y += 28;

    /* ----- editing area ----- */
    i32 area_h = v->y + v->h - y - 54;
    if (area_h < 40) area_h = 40;
    i32 text_w = cw - 132;                 /* the file list is on the right */
    fb_fill(cx + 10, y, text_w, area_h, C_WHITE);
    fb_rect(cx + 10, y, text_w, area_h, fb_rgb(200, 206, 216));

    int vis = area_h / 16;
    if (vis > ED_VIEW) vis = ED_VIEW;

    for (int i = 0; i < vis; i++) {
        int r = ed_top + i;
        if (r >= ed_lines) break;
        i32 ly = y + 4 + i * 16;

        /* line number */
        char num[8];
        ksnprintf(num, sizeof(num), "%u", (u32)(r + 1));
        fb_text(cx + 14, ly, num, fb_rgb(170, 178, 192), 0xFFFFFFFF);

        /* highlight of the current line */
        if (r == ed_cy)
            fb_fill(cx + 44, ly - 1, text_w - 40, 16, fb_rgb(238, 243, 252));

        fb_text(cx + 46, ly, ed_buf[r], C_TEXT, 0xFFFFFFFF);

        /* cursor */
        if (r == ed_cy && !ed_name_mode && ed_blink < 15) {
            i32 curx = cx + 46 + ed_cx * 8;
            fb_fill(curx, ly - 1, 2, 15, C_ACCENT);
        }
    }

    /* a scrollbar when the document does not fit */
    if (ed_lines > vis) {
        i32 track_h = area_h - 8;
        i32 kh = track_h * vis / ed_lines;
        if (kh < 12) kh = 12;
        i32 ky = y + 4 + (track_h - kh) * ed_top / (ed_lines - vis);
        fb_fill(cx + 10 + text_w - 7, y + 4, 5, track_h, fb_rgb(232, 235, 240));
        fb_round_fill(cx + 10 + text_w - 7, ky, 5, kh, fb_rgb(176, 184, 198));
    }

    /* ----- the file list on the right ----- */
    i32 lx = cx + text_w + 18;
    fb_text(lx, y - 18, T("Files:", "Файлы:"), C_TEXT_DIM, 0xFFFFFFFF);
    fb_fill(lx, y, 108, area_h, fb_rgb(250, 251, 253));
    fb_rect(lx, y, 108, area_h, fb_rgb(210, 215, 224));

    rfile_t *tbl = ramfs_table();
    i32 fy = y + 4;
    int shown = 0;
    for (int i = 0; i < RAMFS_MAX_FILES && fy < y + area_h - 16; i++) {
        if (!tbl[i].used) continue;
        int cur = !strcmp(tbl[i].name, ed_name);
        if (cur) fb_fill(lx + 2, fy - 1, 104, 16, fb_rgb(225, 235, 250));
        fb_text(lx + 6, fy, tbl[i].name, cur ? C_ACCENT : C_TEXT, 0xFFFFFFFF);
        hit_add(wid, W_ED_FILE0 + i, lx + 2, fy - 1, 104, 16);
        fy += 16;
        shown++;
    }
    if (!shown) fb_text(lx + 6, y + 4, T("empty", "пусто"), C_TEXT_DIM, 0xFFFFFFFF);

    /* ----- status line ----- */
    i32 sy = v->y + v->h - 24;
    fb_fill(cx + 8, sy - 6, cw - 16, 1, fb_rgb(214, 218, 226));
    char st[80];
    if (ed_status[0] && timer_ticks() < ed_status_until) {
        ksnprintf(st, sizeof(st), "%s", ed_status);
        fb_text(cx + 12, sy, st, C_ACCENT, 0xFFFFFFFF);
    } else {
        ksnprintf(st, sizeof(st), T("Line %u of %u, column %u", "Строка %u из %u, столбец %u"),
                  (u32)(ed_cy + 1), (u32)ed_lines, (u32)(ed_cx + 1));
        fb_text(cx + 12, sy, st, C_TEXT_DIM, 0xFFFFFFFF);
    }
}

/* ============================================================
 *  Programs - installing and launching .kapp applications
 *
 *  Shows which disk was found, whether it is formatted, and the list
 *  of installed programs. They are launched and removed from here too.
 * ============================================================ */
static char store_msg[80];
static u64  store_msg_until = 0;

static void store_say(const char *m) {
    strncpy(store_msg, m, sizeof(store_msg));
    store_msg[sizeof(store_msg) - 1] = 0;
    store_msg_until = timer_ticks() + timer_hz() * 4;
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
    fb_fill(x, y, cw - 24, list_h, fb_rgb(250, 251, 253));
    fb_rect(x, y, cw - 24, list_h, fb_rgb(190, 198, 212));

    for (int i = 0; i < RAMFS_MAX_FILES && shown < (list_h - 8) / 20; i++) {
        if (!tbl[i].used) continue;
        u32 l = (u32)strlen(tbl[i].name);
        if (l < 6 || strcmp(tbl[i].name + l - 5, ".kapp")) continue;   /* .kapp files only */

        i32 iy = y + 4 + shown * 20;
        int running = kapp_loaded() && !strcmp(kapp_filename(), tbl[i].name);
        if (running) fb_fill(x + 2, iy - 1, cw - 28, 19, fb_rgb(214, 232, 252));

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
        fb_fill(x, y, cw - 24, 58, fb_rgb(252, 248, 232));
        fb_rect(x, y, cw - 24, 58, fb_rgb(228, 208, 150));
        fb_text(x + 8, y + 6, T("Install KvantOS onto this disk", "Установить KvantOS на этот диск"),
                fb_rgb(140, 90, 20), 0xFFFFFFFF);

        fb_round_fill(x + 8, y + 26, 130, 24, C_ACCENT);
        fb_text(x + 20, y + 30, T("Install", "Установить"), C_WHITE, 0xFFFFFFFF);
        hit_add(wid, W_ST_SETUP, x + 8, y + 26, 130, 24);

        fb_round_fill(x + 148, y + 26, 190, 24, fb_rgb(226, 232, 240));
        fb_text(x + 158, y + 30, T("Install, keeping files", "Установить, сохранив файлы"), C_TEXT, 0xFFFFFFFF);
        hit_add(wid, W_ST_SETUP_KEEP, x + 148, y + 26, 190, 24);
        y += 66;
    }

    if (!kvfs_mounted()) {
        fb_text(x, y, T("No filesystem on the disk.", "Файловая система не размечена."), C_YELLOW, 0xFFFFFFFF);
        y += 24;
        fb_round_fill(x, y, 210, 26, C_ACCENT);
        fb_text(x + 12, y + 5, T("Format disk (KvFS)", "Разметить диск (KvFS)"), C_WHITE, 0xFFFFFFFF);
        hit_add(wid, W_ST_FORMAT, x, y, 210, 26);
        y += 32;
        fb_text(x, y, T("All data on the disk will be lost.", "Данные на диске будут потеряны."), C_TEXT_DIM, 0xFFFFFFFF);
        y += 26;

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
    fb_fill(x, list_y, cw - 24, list_h, fb_rgb(250, 251, 253));
    fb_rect(x, list_y, cw - 24, list_h, fb_rgb(190, 198, 212));

    int shown = 0;
    char nm[44];
    u32 sz;
    int is_exec;
    for (int i = 0; i < 64 && shown < (list_h - 8) / 20; i++) {
        if (kvfs_list(i, nm, &sz, &is_exec) < 0) break;
        if (!is_exec) continue;                     /* only programs are listed */
        i32 iy = list_y + 4 + shown * 20;

        int running = kapp_loaded() && !strcmp(kapp_filename(), nm);
        if (running) fb_fill(x + 2, iy - 1, cw - 28, 19, fb_rgb(214, 232, 252));

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
    fb_round_fill(x, by, 96, 24, fb_rgb(226, 232, 240));
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
        fb_fill(cx, cy + uh, cw, sh, fb_rgb(238, 242, 248));
        fb_fill(cx, cy + uh, cw, 1, fb_rgb(200, 208, 220));
        const char *st = kapp_status();
        fb_text(cx + 8, cy + uh + 2, (st && st[0]) ? st : kapp_name(),
                C_TEXT_DIM, 0xFFFFFFFF);
    }
}

static void app_help(window_t *v, i32 cx, i32 cy, i32 cw) {
    i32 y = cy + 10;
    fb_text(cx + 12, y, T("KvantGUI controls", "Управление KvantGUI"), C_TEXT, 0xFFFFFFFF); y += 22;
    fb_fill(cx + 12, y, cw - 24, 1, fb_rgb(200, 206, 216)); y += 10;
    const char *rows[] = {
        T("Mouse — drag windows by the title bar", "Мышь — перетаскивайте окна за заголовок"),
        T("The cross in the corner closes a window", "Крестик в углу окна — закрыть"),
        T("Double-click an icon to open a program", "Двойной клик по иконке — открыть программу"),
        T("The bottom panel switches between windows", "Панель снизу — переключение между окнами"),
        T("Settings changes resolution and theme", "В «Настройках» меняются разрешение и тема"),
        "",
        T("Keyboard:", "Клавиатура:"),
        T("  T — open the terminal", "  T — открыть терминал"),
        T("  M — system monitor", "  M — монитор системы"),
        T("  F — file manager", "  F — файловый менеджер"),
        T("  A — about the system", "  A — о системе"),
        T("  P — paint", "  P — рисование"),
        T("  E — notepad (text editor)", "  E — блокнот (редактор текста)"),
        T("  G — programs (install and run)", "  G — программы (установка и запуск)"),
        T("  S — display and appearance settings", "  S — настройки экрана и оформления"),
        T("  L — frame rate cap: off / 60 / 30", "  L — предел частоты кадров: нет / 60 / 30"),
        T("  Esc or Q — leave for the text console", "  Esc или Q — выйти в текстовую консоль"),
        "",
        "",
        T("Notepad:", "Блокнот:"),
        T("  arrows — move the cursor", "  стрелки — перемещение курсора"),
        T("  Ctrl+S — save, Ctrl+N — new file", "  Ctrl+S — сохранить, Ctrl+N — новый"),
        T("  click a file on the right to open it", "  клик по файлу справа — открыть"),
        "",
        T("Programs (the Programs icon):", "Программы (значок «Программы»):"),
        T("  install to disk, run and remove", "  установка на диск, запуск и удаление"),
        T("  applications survive a reboot", "  приложения переживают перезагрузку"),
        T("  how to write your own — see APPS.md", "  как писать свои — файл APPS.md"),
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

    /* shadow */
    if (opt_shadows) fb_fill(v->x + 4, v->y + 4, v->w, v->h, C_SHADOW);
    /* body */
    fb_fill(v->x, v->y, v->w, v->h, C_WIN);
    fb_rect(v->x, v->y, v->w, v->h, C_WINBORDER);

    /* title bar with a gradient */
    if (active) {
        u8 r = accents[theme_accent].r;
        u8 g = accents[theme_accent].g;
        u8 b = accents[theme_accent].b;
        fb_gradient_v(v->x + 1, v->y + 1, v->w - 2, TITLE_H,
                      r, g, b,
                      (u8)(r + (255 - r) / 3),
                      (u8)(g + (255 - g) / 3),
                      (u8)(b + (255 - b) / 3));
    } else {
        fb_fill(v->x + 1, v->y + 1, v->w - 2, TITLE_H, C_TITLE_IN);
    }
    fb_text(v->x + 10, v->y + 4, v->title, C_WHITE, 0xFFFFFFFF);

    /* close button */
    i32 bx = v->x + v->w - 20, by = v->y + 5;
    fb_fill(bx, by, 14, 13, active ? C_RED : fb_rgb(140, 146, 158));
    for (int i = 0; i < 7; i++) {
        fb_pixel((u32)(bx + 4 + i), (u32)(by + 3 + i), C_WHITE);
        fb_pixel((u32)(bx + 10 - i), (u32)(by + 3 + i), C_WHITE);
    }

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
    { 30, 410, APP_PAINT    },
    { 30, 500, APP_SETTINGS },
    { 30, 590, APP_EDIT     },
    { 30, 680, APP_STORE    },
    { 30, 770, APP_HELP     },
};
#define ICON_COUNT ((int)(sizeof(icons) / sizeof(icons[0])))

static int  sel_icon = -1;

static void draw_icon_glyph(int app, i32 x, i32 y) {
    switch (app) {
        case APP_ABOUT:
            fb_fill(x + 8, y, 20, 36, C_CYAN);
            fb_fill(x + 14, y + 6, 8, 4, fb_rgb(20, 30, 50));
            fb_fill(x + 14, y + 14, 8, 16, fb_rgb(20, 30, 50));
            break;
        case APP_SYSMON:
            fb_fill(x, y + 2, 36, 26, fb_rgb(30, 40, 60));
            fb_rect(x, y + 2, 36, 26, C_GREEN);
            for (int i = 0; i < 8; i++)
                fb_fill(x + 3 + i * 4, y + 24 - (i * 2 + 3) % 18, 3, (i * 2 + 3) % 18, C_GREEN);
            fb_fill(x + 12, y + 30, 12, 4, fb_rgb(30, 40, 60));
            break;
        case APP_FILES:
            fb_fill(x, y + 6, 16, 4, C_YELLOW);
            fb_fill(x, y + 10, 36, 24, C_YELLOW);
            fb_fill(x + 3, y + 14, 30, 2, fb_rgb(200, 150, 40));
            break;
        case APP_TERM:
            fb_fill(x, y + 2, 36, 30, fb_rgb(18, 22, 30));
            fb_rect(x, y + 2, 36, 30, fb_rgb(90, 100, 120));
            fb_text(x + 4, y + 10, ">_", C_GREEN, 0xFFFFFFFF);
            break;
        case APP_STORE:
            /* a box with a raised lid - "install a program" */
            fb_fill(x + 2, y + 12, 32, 20, fb_rgb(206, 158, 84));
            fb_fill(x + 2, y + 6,  32, 8,  fb_rgb(232, 186, 112));
            fb_fill(x + 14, y + 6, 8, 26, fb_rgb(170, 126, 62));
            fb_fill(x + 6, y + 16, 24, 2, fb_rgb(150, 110, 54));
            break;
        case APP_USER:
            /* a window with a slider - "a running program" */
            fb_fill(x + 2, y + 4, 32, 26, C_WHITE);
            fb_rect(x + 2, y + 4, 32, 26, fb_rgb(150, 158, 175));
            fb_fill(x + 2, y + 4, 32, 6, C_ACCENT);
            fb_fill(x + 6, y + 16, 14, 3, fb_rgb(120, 130, 150));
            fb_fill(x + 6, y + 22, 20, 3, fb_rgb(120, 130, 150));
            break;
        case APP_EDIT:
            /* a sheet of ruled paper with a pencil */
            fb_fill(x + 4, y + 2, 26, 32, C_WHITE);
            fb_rect(x + 4, y + 2, 26, 32, fb_rgb(150, 158, 175));
            fb_fill(x + 8, y + 8,  18, 2, fb_rgb(120, 130, 150));
            fb_fill(x + 8, y + 14, 18, 2, fb_rgb(120, 130, 150));
            fb_fill(x + 8, y + 20, 12, 2, fb_rgb(120, 130, 150));
            fb_fill(x + 22, y + 22, 10, 10, C_YELLOW);      /* pencil */
            fb_fill(x + 29, y + 29, 4, 4, fb_rgb(60, 50, 40));
            break;
        case APP_PAINT:
            fb_fill(x + 2, y + 4, 32, 26, C_WHITE);
            fb_rect(x + 2, y + 4, 32, 26, fb_rgb(150, 158, 175));
            fb_fill(x + 6, y + 20, 10, 6, C_RED);
            fb_fill(x + 16, y + 12, 8, 14, C_ACCENT);
            fb_fill(x + 24, y + 16, 6, 10, C_GREEN);
            break;
        case APP_SETTINGS: {
            u32 c = fb_rgb(190, 198, 214);
            fb_fill(x + 14, y + 4, 10, 28, c);      /* the cross base */
            fb_fill(x + 4, y + 14, 28, 10, c);
            fb_fill(x + 8, y + 8, 20, 20, c);       /* the gear body */
            fb_fill(x + 13, y + 13, 10, 10, fb_rgb(30, 40, 60));  /* the hole */
            break;
        }
        case APP_HELP:
            fb_fill(x + 6, y + 2, 24, 32, C_WHITE);
            fb_rect(x + 6, y + 2, 24, 32, fb_rgb(150, 158, 175));
            fb_text(x + 14, y + 10, "?", C_ACCENT, 0xFFFFFFFF);
            break;
    }
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
        default:     /* Gradient - a shade of the accent, top to bottom */
            fb_gradient_v(0, 0, (i32)scr_w, dh,
                          (u8)(ar / 2 + 8), (u8)(ag / 2 + 12), (u8)(ab / 2 + 24),
                          8, 12, 24);
    }

    /* the logo in the centre */
    const char *wm = "KvantOS";
    i32 lx = (i32)scr_w / 2 - 100, ly = (i32)scr_h / 2 - 60;
    for (int s = 0; s < 3; s++) {
        const char *t = s == 0 ? "K V A N T" : (s == 1 ? T("desktop environment", "графическая среда") : KV_VERSION);
        i32 tw = (i32)utf8_len(t) * 8;
        fb_text((i32)scr_w / 2 - tw / 2, ly + s * 22, t,
                s == 0 ? fb_rgb((u8)(ar / 2 + 40), (u8)(ag / 2 + 45), (u8)(ab / 2 + 60))
                       : fb_rgb((u8)(ar / 2 + 20), (u8)(ag / 2 + 25), (u8)(ab / 2 + 40)),
                0xFFFFFFFF);
    }
    (void)wm; (void)lx;

    /* Icons: at a low resolution (640x480) the bottom rows did not fit
       and were drawn underneath the taskbar. They are laid out in
       columns according to the available height. */
    /* Both the top margin (50) and the caption height under an icon
       (58) are taken into account, otherwise the bottom row ran into
       the taskbar. */
    i32 usable = dh - 50 - 58;
    int per_col = usable / 90 + 1;
    if (per_col < 1) per_col = 1;

    for (int i = 0; i < ICON_COUNT; i++) {
        i32 x = 30 + (i / per_col) * (ICON_W + 20);
        i32 y = 50 + (i % per_col) * 90;
        icons[i].x = x;
        icons[i].y = y;
        if (i == sel_icon)
            fb_round_fill(x - 6, y - 6, ICON_W, ICON_H, fb_rgb(50, 80, 130));
        draw_icon_glyph(icons[i].app, x + ICON_W / 2 - 24, y);
        fb_text_center(x - 6, y + 44, ICON_W, app_name(icons[i].app), C_WHITE, 0xFFFFFFFF);
    }
}

static u32 gui_fps = 0;      /* frames per second, for measurements */
static u32 gui_fps_limit = 0;  /* 0 = unlimited; otherwise the target fps */

static void draw_panel(void) {
    i32 py = (i32)scr_h - PANEL_H;
    fb_gradient_v(0, py, (i32)scr_w, PANEL_H, 40, 52, 78, 20, 26, 42);
    fb_fill(0, py, (i32)scr_w, 1, fb_rgb(80, 100, 140));

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
        fb_round_fill(bx0, 14, bw, 30, fb_rgb(150, 40, 40));
        fb_rect(bx0, 14, bw, 30, C_WHITE);
        fb_text(bx0 + 12, 22, b, C_WHITE, 0xFFFFFFFF);
    }

    /* the Start button */
    fb_round_fill(6, py + 4, 78, PANEL_H - 8, C_TITLE);
    fb_text(16, py + 10, T("KVANT", "КВАНТ"), C_WHITE, 0xFFFFFFFF);

    /* window buttons */
    i32 bx = 94;
    for (int i = 0; i < win_count; i++) {
        int id = z_order[i];
        if (!wins[id].used) continue;
        int active = (id == top_window());
        fb_round_fill(bx, py + 4, 132, PANEL_H - 8,
                      active ? fb_rgb(70, 90, 130) : fb_rgb(46, 58, 84));
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
        fb_text(bx + 8, py + 10, t, active ? C_WHITE : fb_rgb(190, 198, 214), 0xFFFFFFFF);
        bx += 138;
        if (bx > (i32)scr_w - 260) break;
    }

    /* the clock and statistics on the right */
    rtc_time_t tm;
    rtc_read(&tm);
    char clock[32];
    if (opt_seconds) ksnprintf(clock, sizeof(clock), "%02u:%02u:%02u", tm.hour, tm.min, tm.sec);
    else             ksnprintf(clock, sizeof(clock), "%02u:%02u", tm.hour, tm.min);
    i32 clw = (i32)utf8_len(clock) * 8;
    fb_text((i32)scr_w - clw - 14, py + 10, clock, C_WHITE, 0xFFFFFFFF);

    char mem[32];
    ksnprintf(mem, sizeof(mem), T("RAM %u MiB", "ОЗУ %u МиБ"), pmm_used_frames() * 4096 / 1048576);
    fb_text((i32)scr_w - 190, py + 10, mem, fb_rgb(160, 200, 240), 0xFFFFFFFF);

    /* A frame counter: it exists to measure speed on real hardware
       instead of guessing. Updated once per second. */
    char fps[24];
    ksnprintf(fps, sizeof(fps), T("%u fps", "%u к/с"), gui_fps);
    fb_text((i32)scr_w - 260, py + 10, fps,
            gui_fps >= 25 ? fb_rgb(140, 230, 150) : fb_rgb(240, 200, 120),
            0xFFFFFFFF);
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
    /* if a window of this application already exists, raise it */
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].app == app) {
            wins[i].minimized = 0;
            raise_window(i);
            return;
        }
    i32 w = 420, h = 300;
    if (app == APP_SYSMON) { w = 430; h = 420; }
    if (app == APP_EDIT)   { w = 660; h = 470; }   /* the notepad needs room */
    if (app == APP_TERM)   { w = 480; h = 300; }
    if (app == APP_PAINT)  { w = 390; h = 290; }
    if (app == APP_HELP)   { w = 440; h = 470; }
    if (app == APP_SETTINGS) { w = 470; h = 430; }
    if (app == APP_STORE)  { w = 560; h = 420; }
    if (app == APP_FILES)  { w = 720; h = 480; }   /* two panes need width */
    if (app == APP_USER)   { w = kapp_pref_w() + 2; h = kapp_pref_h() + TITLE_H + 24; }
    i32 x = 190 + (win_count % 4) * 34;
    i32 y = 60 + (win_count % 4) * 28;
    if (x + w > (i32)scr_w) x = (i32)scr_w - w - 10;
    if (y + h > (i32)scr_h - PANEL_H) y = 40;
    win_open(app_name(app), app, x, y, w, h, C_TITLE);
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
    int w = hit_test(id, mx, my);
    if (w < 0) return 0;

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

        static u64 last_tick = 0;
        static int last_row = -1, last_pane = -1;

        fm_set_active(pane);
        fm_pane_set_sel(pane, row);

        u64 now = timer_ticks();
        if (row == last_row && pane == last_pane && now - last_tick < 400)
            fm_activate();
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
        palette_init();
        return 1;
    }
    if (w >= W_WALL && w < W_WALL + WALL_COUNT) { theme_wall = w - W_WALL; return 1; }

    switch (w) {
        case W_APPLY:    settings_apply(); return 1;
        case W_SHADOWS:  opt_shadows = !opt_shadows; return 1;
        case W_SECONDS:  opt_seconds = !opt_seconds; return 1;
        case W_QUIT:     running = 0; return 1;
    }
    return 0;
}

static u64 last_click_tick = 0;
static int last_click_icon = -1;

static void handle_click(i32 mx, i32 my, int pressed) {
    i32 py = (i32)scr_h - PANEL_H;

    if (!pressed) { dragging = -1; return; }

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
            win_close(id);
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
                if (ht->id == W_ST_FORMAT) {
                    int rc = kvfs_format();
                    store_say(rc == 0 ? T("Disk formatted, KvFS ready", "Диск размечен, KvFS готова") : kvfs_error(rc));
                    return;
                }
                if (ht->id == W_ST_REFRESH) {
                    kvfs_mount();
                    store_say(T("List refreshed", "Список обновлён"));
                    return;
                }
                if (ht->id >= W_ST_RAM0) {
                    rfile_t *tbl = ramfs_table();
                    int ri = ht->id - W_ST_RAM0;
                    if (ri >= 0 && ri < RAMFS_MAX_FILES && tbl[ri].used) {
                        for (int q = 0; q < MAX_WIN; q++)
                            if (wins[q].used && wins[q].app == APP_USER) win_close(q);
                        if (kapp_load(tbl[ri].name) == 0) {
                            open_app(APP_USER);
                            int uw = top_window();
                            if (uw >= 0) {
                                strncpy(wins[uw].title, kapp_name(), sizeof(wins[uw].title) - 1);
                                kapp_opened(wins[uw].x + 1, wins[uw].y + TITLE_H + 1,
                                            wins[uw].w - 2, wins[uw].h - TITLE_H - 22);
                            }
                            store_say(T("Started from the image", "Запущено из образа"));
                        } else store_say(kapp_last_error());
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
                        for (int q = 0; q < MAX_WIN; q++)
                            if (wins[q].used && wins[q].app == APP_USER) win_close(q);
                        if (kapp_load(nm) == 0) {
                            open_app(APP_USER);
                            int uw = top_window();
                            if (uw >= 0) {
                                strncpy(wins[uw].title, kapp_name(), sizeof(wins[uw].title) - 1);
                                kapp_opened(wins[uw].x + 1, wins[uw].y + TITLE_H + 1,
                                            wins[uw].w - 2, wins[uw].h - TITLE_H - 22);
                            }
                            store_say(T("Started", "Запущено"));
                        } else {
                            store_say(kapp_last_error());
                        }
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

        /* the Notepad buttons and file list */
        if (v->app == APP_EDIT) {
            for (int k = 0; k < hit_count; k++) {
                hit_t *ht = &hits[k];
                if (ht->win != id) continue;
                if (mx < ht->x || mx >= ht->x + ht->w) continue;
                if (my < ht->y || my >= ht->y + ht->h) continue;

                if (ht->id == W_ED_NEW) {
                    ed_reset();
                    ed_msg(T("New document", "Новый документ"));
                    return;
                }
                if (ht->id == W_ED_SAVE) {
                    /* ask for a name when the file has no meaningful one yet */
                    ed_name_mode = 1;
                    int n = 0;
                    for (; ed_name[n] && n < (int)sizeof(ed_name_in) - 1; n++)
                        ed_name_in[n] = ed_name[n];
                    ed_name_in[n] = 0;
                    ed_msg(T("Type a name and press Enter", "Введите имя и нажмите Enter"));
                    return;
                }
                if (ht->id == W_ED_OPEN) {
                    ed_msg(T("Pick a file from the list on the right", "Выберите файл в списке справа"));
                    return;
                }
                if (ht->id == W_ED_DEL) {
                    if (ramfs_delete(ed_name) == 0) {
                        ed_reset();
                        ed_msg(T("File deleted", "Файл удалён"));
                    } else {
                        ed_msg(T("File not found", "Файл не найден"));
                    }
                    return;
                }
                if (ht->id >= W_ED_FILE0) {
                    rfile_t *tbl = ramfs_table();
                    int fi = ht->id - W_ED_FILE0;
                    if (fi >= 0 && fi < RAMFS_MAX_FILES && tbl[fi].used) {
                        if (ed_load(tbl[fi].name)) ed_msg(T("File opened", "Файл открыт"));
                        else ed_msg(T("Could not open", "Не удалось открыть"));
                    }
                    return;
                }
            }
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
            u64 now = timer_ticks();
            if (last_click_icon == i && now - last_click_tick < timer_hz() * 9 / 10)
                open_app(icons[i].app);       /* double click */
            sel_icon = i;
            last_click_icon = i;
            last_click_tick = now;
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

static void handle_key(int c) {
    int top = top_window();

    /* ----- An application on top: keys go to it -----
       Esc is kept for ourselves, otherwise a hung program would be
       impossible to leave. */
    if (top >= 0 && wins[top].app == APP_USER && kapp_loaded() && c != 27) {
        window_t *v = &wins[top];
        kapp_key(c, v->x + 1, v->y + TITLE_H + 1,
                 v->w - 2, v->h - TITLE_H - 2 - 20);
        return;
    }

    /* ----- The file manager on top: it handles navigation itself.
       Esc is only ours once no dialog is open inside it. ----- */
    if (top >= 0 && wins[top].app == APP_FILES) {
        window_t *v = &wins[top];
        int rows = fm_rows_visible(v);
        if (c == 27 && !fm_view_is_open() && !fm_confirm_pending() && !fm_input_active()) {
            /* fall through: Esc closes the window */
        } else if (fm_key(c, rows)) {
            return;
        }
    }

    /* ----- Notepad on top: the editor takes all input ----- */
    if (top >= 0 && wins[top].app == APP_EDIT) {
        /* file name input mode */
        if (ed_name_mode) {
            int len = 0; while (ed_name_in[len]) len++;
            if (c == '\n') {
                if (len) {
                    for (int i = 0; i <= len; i++) ed_name[i] = ed_name_in[i];
                    ed_name_mode = 0;
                    ed_save();
                } else {
                    ed_name_mode = 0;
                    ed_msg(T("No name given", "Имя не задано"));
                }
                return;
            }
            if (c == 27) { ed_name_mode = 0; ed_msg(T("Cancelled", "Отменено")); return; }
            if (c == '\b') { if (len) ed_name_in[len - 1] = 0; return; }
            if (c >= 32 && c < 127 && len < (int)sizeof(ed_name_in) - 1) {
                ed_name_in[len] = (char)c;
                ed_name_in[len + 1] = 0;
            }
            return;
        }

        switch (c) {
            case 27:                       /* Esc - close the window */
                win_close(top);
                return;
            case '\n': ed_newline();      ed_scroll_to_cursor(); return;
            case '\b': ed_backspace();    ed_scroll_to_cursor(); return;
            case KEY_LEFT:
                if (ed_cx > 0) ed_cx--;
                else if (ed_cy > 0) {
                    ed_cy--;
                    ed_cx = 0; while (ed_buf[ed_cy][ed_cx]) ed_cx++;
                }
                ed_scroll_to_cursor();
                return;
            case KEY_RIGHT: {
                int len = 0; while (ed_buf[ed_cy][len]) len++;
                if (ed_cx < len) ed_cx++;
                else if (ed_cy < ed_lines - 1) { ed_cy++; ed_cx = 0; }
                ed_scroll_to_cursor();
                return;
            }
            case KEY_UP:
                if (ed_cy > 0) {
                    ed_cy--;
                    int len = 0; while (ed_buf[ed_cy][len]) len++;
                    if (ed_cx > len) ed_cx = len;
                }
                ed_scroll_to_cursor();
                return;
            case KEY_DOWN:
                if (ed_cy < ed_lines - 1) {
                    ed_cy++;
                    int len = 0; while (ed_buf[ed_cy][len]) len++;
                    if (ed_cx > len) ed_cx = len;
                }
                ed_scroll_to_cursor();
                return;
            case 19:                       /* Ctrl+S - save */
                /* Ask for a name, otherwise the document was silently
                   saved under the default name. */
                ed_name_mode = 1;
                {
                    int n = 0;
                    for (; ed_name[n] && n < (int)sizeof(ed_name_in) - 1; n++)
                        ed_name_in[n] = ed_name[n];
                    ed_name_in[n] = 0;
                }
                ed_msg(T("File name then Enter (Esc cancels)", "Имя файла и Enter (Esc - отмена)"));
                return;
            case 14:                       /* Ctrl+N - new document */
                ed_reset();
                ed_msg(T("New document", "Новый документ"));
                return;
            case '\t':
                for (int i = 0; i < 4; i++) ed_insert_char(' ');
                return;
        }
        if (c >= 32 && c < 127) { ed_insert_char((char)c); ed_scroll_to_cursor(); }
        return;
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
        if (c == 27) { running = 0; return; }
        if (c >= 32 && c < 127 && term_len < TERM_COLS - 10) {
            term_input[term_len++] = (char)c;
            term_input[term_len] = 0;
            return;
        }
        return;
    }

    switch (c) {
        case 27: case 'q': case 'Q': running = 0; break;
        case '\n': case ' ':
            if (sel_icon >= 0 && sel_icon < ICON_COUNT) open_app(icons[sel_icon].app);
            break;
        case KEY_DOWN:
            sel_icon = (sel_icon + 1) % ICON_COUNT; break;
        case KEY_UP:
            sel_icon = (sel_icon <= 0 ? ICON_COUNT : sel_icon) - 1; break;
        case 't': case 'T': open_app(APP_TERM); break;
        case 'm': case 'M': open_app(APP_SYSMON); break;
        case 'f': case 'F': open_app(APP_FILES); break;
        case 'a': case 'A': open_app(APP_ABOUT); break;
        case 'g': case 'G': open_app(APP_STORE); break;
        case 'p': case 'P': open_app(APP_PAINT); break;
        case 'e': case 'E': open_app(APP_EDIT); break;
        case 'h': case 'H': case '?': open_app(APP_HELP); break;
        /* The L key toggles the frame rate cap.
           Without a cap the picture is as smooth as possible but the
           CPU is fully loaded - on a laptop that drains the battery. */
        case 'l': case 'L':
            if      (gui_fps_limit == 0)  gui_fps_limit = 60;
            else if (gui_fps_limit == 60) gui_fps_limit = 30;
            else                          gui_fps_limit = 0;
            break;
        case 's': case 'S': open_app(APP_SETTINGS); break;
        case 'w': case 'W': {
            int top2 = top_window();
            if (top2 >= 0) win_close(top2);
            break;
        }
    }
}

/* ---------- main loop ---------- */

int gui_run(void) {
    if (!fb_active()) return -1;

    scr_w = fb_width();
    scr_h = fb_height();
    palette_init();

    u32 bytes = fb_pitch_get() * scr_h;
    backbuf = (u32 *)kmalloc(bytes);
    /* Without a backbuffer every primitive goes straight to uncached
       video memory - on real hardware that is unusably slow and looks
       like a freeze. Better to refuse the desktop than to hang it. */
    if (!backbuf) return -2;
    fb_set_target(backbuf);

    memset(wins, 0, sizeof(wins));
    win_count = 0; dragging = -1; sel_icon = -1;
    memset(paint_grid, 0, sizeof(paint_grid));
    term_row = 0; term_len = 0; term_input[0] = 0;
    memset(term_buf, 0, sizeof(term_buf));
    term_putline(T("KvantGUI terminal. Type help.", "KvantGUI терминал. Наберите help."));

    mouse_set_bounds((i32)scr_w, (i32)scr_h);
    mouse_set_pos((i32)scr_w / 2, (i32)scr_h / 2);

    /* Tracking the changed screen area. A full frame blit (3 MiB at
       1024x768) is the most expensive operation in the loop, so only
       the band of rows that actually changed is sent to video memory:
       windows, the panel, the cursor. */
    int full_redraw = 1;                 /* the first frame is blitted in full */
    int frame_tick = 0;
    u32 fps_frames = 0;
    u64 fps_mark = timer_ticks();
    i32 prev_mx = mouse_x(), prev_my = mouse_y();
    (void)prev_mx;

    open_app(APP_ABOUT);

    running = 1;
    int prev_btn = 0;

    while (running) {
        /* keyboard input */
        int c;
        while ((c = kbd_getchar_nb()) >= 0) {
            /* Any deliberate keystroke proves the screen is readable:
               Enter/Y confirms the new mode, Esc undoes it at once. */
            if (revert_armed) {
                if (c == 27 || c == 'n' || c == 'N') { mode_revert_now(); full_redraw = 1; continue; }
                revert_armed = 0;
                set_status(T("Resolution confirmed", "Разрешение подтверждено"), C_GREEN);
                if (c == '\n' || c == '\r' || c == 'y' || c == 'Y') { full_redraw = 1; continue; }
            }
            handle_key(c); full_redraw = 1;
        }

        /* the user never confirmed - the screen is probably blank */
        if (revert_armed && timer_ticks() >= revert_deadline) {
            mode_revert_now();
            full_redraw = 1;
        }

        /* mouse */
        i32 mx = mouse_x(), my = mouse_y();
        int btn = mouse_buttons() & 1;
        if (btn && !prev_btn) {
            if (revert_armed) {
                revert_armed = 0;
                set_status(T("Resolution confirmed", "Разрешение подтверждено"), C_GREEN);
            }
            handle_click(mx, my, 1); full_redraw = 1;
        }
        else if (!btn && prev_btn) { handle_click(mx, my, 0); full_redraw = 1; }
        else if (btn && prev_btn) {
            handle_drag(mx, my);
            full_redraw = 1;
            int top = top_window();
            if (dragging < 0 && top >= 0 && wins[top].app == APP_PAINT)
                handle_click(mx, my, 1);      /* continuous drawing */
        }
        prev_btn = btn;

        /* frame */
        u64 frame_start = timer_ticks();
        hit_reset();
        draw_desktop();
        int top = top_window();
        for (int i = 0; i < win_count; i++) draw_window(z_order[i], z_order[i] == top);
        draw_panel();
        draw_cursor(mx, my);

        if (backbuf) {
            if (full_redraw) {
                fb_present(backbuf);          /* the contents changed entirely */
            } else {
                /* Nothing changed structurally: only the cursor band
                   (its old and new place) and the panel with the clock
                   are sent to video memory. That is tens of kilobytes
                   instead of three megabytes. */
                i32 cy0 = (my < prev_my ? my : prev_my) - 2;
                i32 cy1 = (my > prev_my ? my : prev_my) + 22;
                if (cy0 < 0) cy0 = 0;
                if (cy1 > (i32)fb_height()) cy1 = (i32)fb_height();
                if (cy1 > cy0) fb_present_rows(backbuf, (u32)cy0, (u32)cy1);

                i32 py = (i32)fb_height() - PANEL_H;
                if (py < 0) py = 0;
                fb_present_rows(backbuf, (u32)py, fb_height());
            }
        }
        prev_mx = mx; prev_my = my;
        full_redraw = 0;

        /* The Monitor window shows live counters and the panel clock
           ticks on its own. Every 8 frames (roughly a quarter of a
           second) the whole screen is blitted so that such changes do
           not freeze. */
        if (++frame_tick >= 8) { frame_tick = 0; full_redraw = 1; }

        /* Measuring the frame rate with the system timer (100 Hz). */
        fps_frames++;
        u64 now = timer_ticks();
        u32 hz = timer_hz();
        if (now - fps_mark >= hz) {
            u32 span = (u32)(now - fps_mark);      /* about a second, it fits */
            gui_fps = span ? (fps_frames * hz / span) : 0;
            fps_frames = 0;
            fps_mark = now;
        }

        /* There is no frame rate cap: we draw as fast as the hardware
           allows. Control is still yielded to the scheduler, otherwise
           background tasks would never get the CPU.
           gui_fps_limit = 0 means unlimited, otherwise it is the
           target fps. */
        if (gui_fps_limit) {
            u32 hz     = timer_hz();
            u32 budget = hz / gui_fps_limit;                   /* ticks per frame */
            u32 spent  = (u32)(timer_ticks() - frame_start);   /* always too few */
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

    fb_set_target(NULL);
    if (backbuf) { kfree(backbuf); backbuf = NULL; }
    return 0;
}
