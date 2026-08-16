/* ============================================================
 *  KvantOS - the file manager
 *
 *  A two-pane manager in the spirit of Norton Commander: the left pane
 *  is the source, the right one the destination, Tab switches between
 *  them. That layout makes copying obvious - it is always "from the
 *  active pane to the other one" - and it fits every mounted volume,
 *  whether that is RAM, KvFS, FAT32 or NTFS.
 *
 *  The drawing lives here, but the window frame, the hit testing and
 *  the event loop belong to gui.c, which calls into these functions.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"
#include "filemgr.h"

#define FM_MAX_ENTRIES  512
#define ROW_H           18

typedef struct {
    char path[VFS_MAX_PATH];
    vfs_dirent_t items[FM_MAX_ENTRIES];
    int  count;
    int  sel;
    int  scroll;
    int  loaded;
    /* Free space is cached per pane. Asking the driver costs a scan of
       the FAT or of the NTFS bitmap - hundreds of sectors over PIO. The
       redraw runs every frame, so querying it there dropped the whole
       desktop to two frames per second. It is refreshed only when the
       directory is re-read. */
    u32  total_kb, free_kb;
} fm_pane_t;

static fm_pane_t panes[2];
static int  fm_active = 0;                  /* 0 = left, 1 = right */
static char fm_status[160];
static u32  fm_status_col;
static int  fm_inited = 0;

/* the preview window for the contents of a file */
static int  fm_view_open = 0;
static char fm_view_name[VFS_MAX_NAME];
static char fm_view_buf[4096];
static int  fm_view_len = 0;
static int  fm_view_scroll = 0;
static int  fm_view_binary = 0;

/* confirmation before a destructive action */
static int  fm_confirm = FM_CONFIRM_NONE;
static char fm_confirm_arg[VFS_MAX_PATH];

/* text input (a new directory name) */
static int  fm_input_mode = 0;
static char fm_input[VFS_MAX_NAME];

void fm_say(const char *msg, u32 colour) {
    strncpy(fm_status, msg, sizeof(fm_status));
    fm_status_col = colour;
}

const char *fm_status_text(void) { return fm_status; }
u32         fm_status_colour(void) { return fm_status_col; }
int         fm_view_is_open(void) { return fm_view_open; }
int         fm_confirm_pending(void) { return fm_confirm; }
int         fm_input_active(void) { return fm_input_mode; }

/* ---------- sorting: directories first, then names ---------- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int name_before(const vfs_dirent_t *a, const vfs_dirent_t *b) {
    if (a->is_dir != b->is_dir) return a->is_dir > b->is_dir;
    const char *x = a->name, *y = b->name;
    while (*x && *y) {
        char cx = lower(*x), cy = lower(*y);
        if (cx != cy) return cx < cy;
        x++; y++;
    }
    return *x == 0 && *y != 0;
}

static void sort_pane(fm_pane_t *p) {
    /* insertion sort: a directory listing is short and this keeps the
       code small - no recursion, no scratch buffer */
    for (int i = 1; i < p->count; i++) {
        vfs_dirent_t key = p->items[i];
        int j = i - 1;
        while (j >= 0 && name_before(&key, &p->items[j])) {
            p->items[j + 1] = p->items[j];
            j--;
        }
        p->items[j + 1] = key;
    }
}

/* ---------- reading a directory ---------- */

static void pane_load(fm_pane_t *p) {
    vfs_space(p->path, &p->total_kb, &p->free_kb);
    p->count = 0;
    for (int i = 0; i < FM_MAX_ENTRIES; i++) {
        if (!vfs_readdir(p->path, i, &p->items[p->count])) break;
        p->count++;
    }
    sort_pane(p);
    p->loaded = 1;
    if (p->sel >= p->count) p->sel = p->count ? p->count - 1 : 0;
    if (p->sel < 0) p->sel = 0;
}

static void pane_goto(fm_pane_t *p, const char *path) {
    strncpy(p->path, path, sizeof(p->path));
    p->sel = 0;
    p->scroll = 0;
    pane_load(p);
}

void fm_refresh(void) {
    pane_load(&panes[0]);
    pane_load(&panes[1]);
}

/* The very first volume that is not the RAM disk, so the manager opens
   on something interesting when a real disk is present. */
static void default_path(char *dst, u32 dstsz, int prefer_second) {
    int n = vfs_volume_count();
    int chosen = -1, first_disk = -1;

    for (int i = 0; i < n; i++) {
        vfs_volume_t *v = vfs_volume(i);
        if (v->kind == FS_RAMFS) continue;
        if (first_disk < 0) first_disk = i;
        else if (prefer_second && chosen < 0) chosen = i;
    }
    if (chosen < 0) chosen = first_disk;
    if (chosen < 0) chosen = 0;
    if (!n) { strncpy(dst, "/mnt/ram", dstsz); return; }

    vfs_volume_t *v = vfs_volume(chosen);
    ksnprintf(dst, dstsz, "/mnt/%s", v->name);
}

void fm_init(void) {
    if (fm_inited) return;
    fm_inited = 1;

    char a[VFS_MAX_PATH], b[VFS_MAX_PATH];
    default_path(a, sizeof(a), 0);
    default_path(b, sizeof(b), 1);

    pane_goto(&panes[0], a);
    pane_goto(&panes[1], b);
    fm_say(T("Tab - switch pane, Enter - open, F5 - copy, F8 - delete",
             "Tab — панель, Enter — открыть, F5 — копировать, F8 — удалить"), 0);
}

/* ---------- actions ---------- */

static fm_pane_t *act(void)   { return &panes[fm_active]; }
static fm_pane_t *other(void) { return &panes[fm_active ^ 1]; }

static void selected_path(char *dst, u32 dstsz) {
    fm_pane_t *p = act();
    if (p->sel < 0 || p->sel >= p->count) { dst[0] = 0; return; }
    vfs_join(dst, dstsz, p->path, p->items[p->sel].name);
}

/* Enter: descend into a directory, or preview a file. */
static void fm_enter(void) {
    fm_pane_t *p = act();
    if (!p->count) return;

    vfs_dirent_t *e = &p->items[p->sel];
    char full[VFS_MAX_PATH];
    vfs_join(full, sizeof(full), p->path, e->name);

    if (e->is_dir) { pane_goto(p, full); return; }

    /* a file: show the beginning of it */
    int n = vfs_read(full, 0, fm_view_buf, sizeof(fm_view_buf) - 1);
    if (n < 0) {
        fm_say(T("Cannot read the file", "Не удалось прочитать файл"), 0xFFCC3344);
        return;
    }
    fm_view_len = n;
    fm_view_buf[n] = 0;

    /* Decide whether this is text: control bytes other than the usual
       whitespace mean we should show hex instead of mojibake. */
    int weird = 0;
    for (int i = 0; i < n; i++) {
        u8 c = (u8)fm_view_buf[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 32 || c == 127) weird++;
    }
    fm_view_binary = (n > 0 && weird * 20 > n);

    strncpy(fm_view_name, e->name, sizeof(fm_view_name));
    fm_view_scroll = 0;
    fm_view_open = 1;
}

static void fm_updir(void) {
    fm_pane_t *p = act();
    if (vfs_is_root(p->path)) {
        /* At the top of a volume Backspace moves to the next volume:
           that is how you reach another disk without a path prompt. */
        fm_next_volume();
        return;
    }
    char up[VFS_MAX_PATH];
    vfs_parent(up, sizeof(up), p->path);
    pane_goto(p, up);
}

void fm_next_volume(void) {
    fm_pane_t *p = act();
    int n = vfs_volume_count();
    if (n < 1) return;

    /* which volume is the pane on now? */
    int cur = 0;
    for (int i = 0; i < n; i++) {
        vfs_volume_t *v = vfs_volume(i);
        char pref[VFS_MAX_PATH];
        ksnprintf(pref, sizeof(pref), "/mnt/%s", v->name);
        u32 l = strlen(pref);
        if (strncmp(p->path, pref, l) == 0 && (p->path[l] == 0 || p->path[l] == '/')) {
            cur = i; break;
        }
    }
    vfs_volume_t *nv = vfs_volume((cur + 1) % n);
    char np[VFS_MAX_PATH];
    ksnprintf(np, sizeof(np), "/mnt/%s", nv->name);
    pane_goto(p, np);

    char msg[96];
    ksnprintf(msg, sizeof(msg), T("Volume %s (%s)", "Том %s (%s)"),
              nv->name, vfs_kind_name(nv->kind));
    fm_say(msg, 0);
}

/* Copy the selected file to the other pane. */
static void fm_copy(void) {
    fm_pane_t *src = act(), *dst = other();
    if (!src->count) return;

    vfs_dirent_t *e = &src->items[src->sel];
    if (e->is_dir) {
        fm_say(T("Copying whole directories is not supported yet",
                 "Копирование каталогов целиком пока не поддерживается"), 0xFFCC7711);
        return;
    }

    char from[VFS_MAX_PATH], to[VFS_MAX_PATH];
    vfs_join(from, sizeof(from), src->path, e->name);
    vfs_join(to,   sizeof(to),   dst->path, e->name);

    if (!vfs_writable(to)) {
        fm_say(T("The target volume is read-only", "Целевой том только для чтения"), 0xFFCC3344);
        return;
    }

    u32 size = vfs_size(from);
    if (size > FM_COPY_LIMIT) {
        char m[128];
        ksnprintf(m, sizeof(m),
                  T("The file is larger than %u KiB - too big to copy",
                    "Файл больше %u КиБ — слишком велик для копирования"),
                  (u32)(FM_COPY_LIMIT / 1024));
        fm_say(m, 0xFFCC3344);
        return;
    }

    u8 *buf = (u8 *)kmalloc(size ? size : 1);
    if (!buf) { fm_say(T("Not enough memory", "Недостаточно памяти"), 0xFFCC3344); return; }

    int got = vfs_read(from, 0, buf, size);
    if (got < 0) {
        kfree(buf);
        fm_say(T("Read error", "Ошибка чтения"), 0xFFCC3344);
        return;
    }

    int wrote = vfs_write(to, buf, (u32)got);
    kfree(buf);

    if (wrote == (int)got) {
        char m[160];
        ksnprintf(m, sizeof(m), T("Copied: %s (%u bytes)", "Скопировано: %s (%u байт)"),
                  e->name, (u32)got);
        fm_say(m, 0xFF2E8B57);
        pane_load(dst);
    } else if (wrote == -2) {
        fm_say(T("The target volume is read-only", "Целевой том только для чтения"), 0xFFCC3344);
    } else if (wrote == -3) {
        fm_say(T("Not enough space on the target volume", "На целевом томе нет места"), 0xFFCC3344);
    } else {
        fm_say(T("Write error", "Ошибка записи"), 0xFFCC3344);
    }
}

static void fm_do_delete(const char *path) {
    int r = vfs_remove(path);
    if (r == 0) {
        fm_say(T("Deleted", "Удалено"), 0xFF2E8B57);
        pane_load(act());
        pane_load(other());
    } else if (r == -2) {
        fm_say(T("This volume is read-only", "Этот том только для чтения"), 0xFFCC3344);
    } else if (r == -4) {
        fm_say(T("The directory is not empty", "Каталог не пуст"), 0xFFCC7711);
    } else {
        fm_say(T("Could not delete", "Не удалось удалить"), 0xFFCC3344);
    }
}

static void fm_delete_ask(void) {
    fm_pane_t *p = act();
    if (!p->count) return;

    char full[VFS_MAX_PATH];
    selected_path(full, sizeof(full));
    if (!full[0]) return;

    if (!vfs_writable(full)) {
        fm_say(T("This volume is read-only", "Этот том только для чтения"), 0xFFCC3344);
        return;
    }
    strncpy(fm_confirm_arg, full, sizeof(fm_confirm_arg));
    fm_confirm = FM_CONFIRM_DELETE;
}

static void fm_mkdir_commit(void) {
    fm_pane_t *p = act();
    if (!fm_input[0]) { fm_say(T("No name given", "Имя не задано"), 0xFFCC7711); return; }

    char full[VFS_MAX_PATH];
    vfs_join(full, sizeof(full), p->path, fm_input);

    int r = vfs_mkdir(full);
    if (r == 0)       { fm_say(T("Directory created", "Каталог создан"), 0xFF2E8B57); pane_load(p); }
    else if (r == -2) fm_say(T("This volume is read-only", "Этот том только для чтения"), 0xFFCC3344);
    else if (r == -5) fm_say(T("Such a name already exists", "Такое имя уже существует"), 0xFFCC7711);
    else              fm_say(T("Could not create the directory", "Не удалось создать каталог"), 0xFFCC3344);
}

/* ---------- input ---------- */

int fm_key(int c, int visible_rows) {
    /* a name being typed */
    if (fm_input_mode) {
        int len = (int)strlen(fm_input);
        if (c == '\n') { fm_input_mode = 0; fm_mkdir_commit(); return 1; }
        if (c == 27)   { fm_input_mode = 0; fm_say(T("Cancelled", "Отменено"), 0); return 1; }
        if (c == '\b') { if (len) fm_input[len - 1] = 0; return 1; }
        if (c >= 32 && c < 127 && len < (int)sizeof(fm_input) - 1) {
            fm_input[len] = (char)c;
            fm_input[len + 1] = 0;
        }
        return 1;
    }

    /* waiting for a yes/no answer */
    if (fm_confirm) {
        if (c == 'y' || c == 'Y' || c == '\n') {
            if (fm_confirm == FM_CONFIRM_DELETE) fm_do_delete(fm_confirm_arg);
            fm_confirm = FM_CONFIRM_NONE;
            return 1;
        }
        if (c == 'n' || c == 'N' || c == 27) {
            fm_confirm = FM_CONFIRM_NONE;
            fm_say(T("Cancelled", "Отменено"), 0);
            return 1;
        }
        return 1;
    }

    /* the preview window swallows keys while it is open */
    if (fm_view_open) {
        int lines = fm_view_binary ? (fm_view_len + 15) / 16 : 0;
        if (!lines) { lines = 1; for (int i = 0; i < fm_view_len; i++) if (fm_view_buf[i] == '\n') lines++; }
        switch (c) {
            case 27: case 'q': case 'Q': fm_view_open = 0; return 1;
            case KEY_UP:   if (fm_view_scroll) fm_view_scroll--; return 1;
            case KEY_DOWN: if (fm_view_scroll < lines - 4) fm_view_scroll++; return 1;
            case KEY_PGUP: fm_view_scroll -= 10; if (fm_view_scroll < 0) fm_view_scroll = 0; return 1;
            case KEY_PGDN:
                fm_view_scroll += 10;
                if (fm_view_scroll > lines - 4) fm_view_scroll = lines - 4;
                if (fm_view_scroll < 0) fm_view_scroll = 0;
                return 1;
        }
        return 1;
    }

    fm_pane_t *p = act();
    if (visible_rows < 1) visible_rows = 1;

    switch (c) {
        case '\t':
            fm_active ^= 1;
            return 1;

        case KEY_UP:
            if (p->sel > 0) p->sel--;
            break;
        case KEY_DOWN:
            if (p->sel < p->count - 1) p->sel++;
            break;
        case KEY_PGUP:
            p->sel -= visible_rows;
            if (p->sel < 0) p->sel = 0;
            break;
        case KEY_PGDN:
            p->sel += visible_rows;
            if (p->sel > p->count - 1) p->sel = p->count - 1;
            if (p->sel < 0) p->sel = 0;
            break;
        case KEY_HOME: p->sel = 0; break;
        case KEY_END:  p->sel = p->count ? p->count - 1 : 0; break;

        case '\n':  fm_enter();  return 1;
        case '\b':  fm_updir();  return 1;

        case KEY_F3: fm_enter(); return 1;
        case KEY_F5: fm_copy();  return 1;
        case KEY_F7:
            if (!vfs_writable(p->path)) {
                fm_say(T("This volume is read-only", "Этот том только для чтения"), 0xFFCC3344);
                return 1;
            }
            fm_input_mode = 1;
            fm_input[0] = 0;
            return 1;
        case KEY_F8: fm_delete_ask(); return 1;
        case KEY_F2: fm_refresh(); fm_say(T("Refreshed", "Обновлено"), 0); return 1;

        default:
            return 0;
    }

    /* keep the selection inside the visible window */
    if (p->sel < p->scroll) p->scroll = p->sel;
    if (p->sel >= p->scroll + visible_rows) p->scroll = p->sel - visible_rows + 1;
    if (p->scroll < 0) p->scroll = 0;
    return 1;
}

/* ---------- accessors used by the drawing code in gui.c ---------- */

int  fm_active_pane(void) { return fm_active; }
void fm_set_active(int i) { fm_active = i & 1; }

const char *fm_pane_path(int i)  { return panes[i & 1].path; }
int  fm_pane_count(int i)        { return panes[i & 1].count; }
int  fm_pane_sel(int i)          { return panes[i & 1].sel; }
int  fm_pane_scroll(int i)       { return panes[i & 1].scroll; }

void fm_pane_set_sel(int i, int sel) {
    fm_pane_t *p = &panes[i & 1];
    if (sel >= 0 && sel < p->count) p->sel = sel;
}

void fm_pane_scroll_by(int i, int delta, int visible_rows) {
    fm_pane_t *p = &panes[i & 1];
    p->scroll += delta;
    int maxs = p->count - visible_rows;
    if (maxs < 0) maxs = 0;
    if (p->scroll > maxs) p->scroll = maxs;
    if (p->scroll < 0) p->scroll = 0;
}

void fm_pane_space(int i, u32 *total_kb, u32 *free_kb) {
    fm_pane_t *p = &panes[i & 1];
    if (total_kb) *total_kb = p->total_kb;
    if (free_kb)  *free_kb  = p->free_kb;
}

vfs_dirent_t *fm_pane_item(int i, int index) {
    fm_pane_t *p = &panes[i & 1];
    if (index < 0 || index >= p->count) return NULL;
    return &p->items[index];
}

/* Open whatever is selected: shared by Enter and the double click. */
void fm_activate(void) { fm_enter(); }
void fm_go_up(void)    { fm_updir(); }
void fm_do_copy(void)  { fm_copy(); }
void fm_ask_delete(void) { fm_delete_ask(); }
void fm_ask_mkdir(void) {
    if (!vfs_writable(panes[fm_active].path)) {
        fm_say(T("This volume is read-only", "Этот том только для чтения"), 0xFFCC3344);
        return;
    }
    fm_input_mode = 1;
    fm_input[0] = 0;
}
void fm_close_view(void) { fm_view_open = 0; }
void fm_confirm_yes(void) {
    if (fm_confirm == FM_CONFIRM_DELETE) fm_do_delete(fm_confirm_arg);
    fm_confirm = FM_CONFIRM_NONE;
}
void fm_confirm_no(void) { fm_confirm = FM_CONFIRM_NONE; }

const char *fm_confirm_target(void) { return fm_confirm_arg; }
const char *fm_input_text(void) { return fm_input; }
const char *fm_view_title(void) { return fm_view_name; }
const char *fm_view_data(void) { return fm_view_buf; }
int  fm_view_length(void) { return fm_view_len; }
int  fm_view_is_binary(void) { return fm_view_binary; }
int  fm_view_scroll_pos(void) { return fm_view_scroll; }
void fm_view_scroll_by(int d) {
    fm_view_scroll += d;
    if (fm_view_scroll < 0) fm_view_scroll = 0;
}
