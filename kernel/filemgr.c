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
#include "native_name.h"

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
static u32 pane_namespace[2];
static int  fm_active = 0;                  /* 0 = left, 1 = right */
static char fm_status[160];
static u32  fm_status_col;
static int  fm_inited = 0;
static u32 listing_epoch;
u32 fm_listing_epoch(void) {return listing_epoch;}
int fm_listing_current(int i) {return i>=0 && i<2 && panes[i].loaded && pane_namespace[i] && pane_namespace[i]==vfs_generation();}
static int require_listing(int i) {
    if(fm_listing_current(i))return 1;
    fm_say(T("Listing changed or unavailable; refresh", "Список изменился или недоступен; обновите"),0xFFCC3344);return 0;
}

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
static u32 fm_confirm_namespace,fm_confirm_listing;
static int fm_confirm_pane;

/* A modal name edit is bound to its original pane/listing/namespace. */
enum {FM_INPUT_MKDIR=1,FM_INPUT_RENAME=2};
static int  fm_input_mode = 0;
static char fm_input[VFS_MAX_NAME];
static char fm_input_dir[VFS_MAX_PATH],fm_input_from[VFS_MAX_PATH];
static u32 fm_input_listing,fm_input_namespace;
static int fm_input_pane,fm_input_replace;

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
    u32 epoch=vfs_generation();
    vfs_space(p->path,&p->total_kb,&p->free_kb);
    p->count=0;int valid=epoch && epoch==vfs_generation();
    for(int i=0;valid && i<FM_MAX_ENTRIES;i++) {
        vfs_dirent_t *e=&p->items[p->count];memset(e,0,sizeof(*e));
        int r=vfs_readdir(p->path,i,e);
        if(epoch!=vfs_generation() || r<0 || r>1){valid=0;break;}
        if(!r)break;
        u32 n=0;while(n<sizeof(e->name) && e->name[n])n++;
        if(n==sizeof(e->name) || !n){valid=0;break;}
        p->count++;
    }
    if(!valid){p->count=0;p->total_kb=p->free_kb=0;p->scroll=0;}
    else sort_pane(p);
    p->loaded=valid;pane_namespace[p==&panes[1]]=valid?epoch:0;
    if(!++listing_epoch)listing_epoch++;
    if(p->sel>=p->count)p->sel=p->count?p->count-1:0;
    if(p->sel<0)p->sel=0;
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

/* Never turn a truncated join into a different source/target file. */
static int fm_path(char out[VFS_MAX_PATH],const char *dir,const char *name) {
    u32 n=0,m=0;while(n<VFS_MAX_PATH && dir[n])n++;while(m<VFS_MAX_NAME && name[m])m++;
    if(n==VFS_MAX_PATH || m==VFS_MAX_NAME || !m || !strcmp(name,".") || !strcmp(name,"..") ||
       n+m+(n && dir[n-1]!='/')>=VFS_MAX_PATH)return 0;
    for(u32 i=0;i<m;i++)if(name[i]=='/' || name[i]=='\\')return 0;
    vfs_join(out,VFS_MAX_PATH,dir,name);return 1;
}
static int selected_path(char out[VFS_MAX_PATH]) {
    fm_pane_t *p=act();
    if(!require_listing(fm_active) || p->sel<0 || p->sel>=p->count || p->sel>=FM_MAX_ENTRIES)return 0;
    if(!fm_path(out,p->path,p->items[p->sel].name)) {
        fm_say(T("Invalid name or path too long", "Неверное имя или слишком длинный путь"),0xFFCC3344);return 0;
    }
    return 1;
}

/* Enter: descend into a directory, or preview a file. */
static void fm_enter(void) {
    fm_pane_t *p=act();char full[VFS_MAX_PATH];
    if(!selected_path(full))return;
    vfs_dirent_t *e=&p->items[p->sel];u32 epoch=pane_namespace[fm_active];

    if (e->is_dir) { pane_goto(p, full); return; }

    /* a file: show the beginning of it */
    int n = vfs_read(full, 0, fm_view_buf, sizeof(fm_view_buf) - 1);
    if(n<0 || (u32)n>=sizeof(fm_view_buf) || epoch!=vfs_generation()) {
        fm_view_len=0;fm_view_open=0;
        fm_say(T("Read failed or storage changed", "Ошибка чтения или смена хранилища"),0xFFCC3344);
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

/* Copy the selected file to the other pane. Source is never removed. */
static void fm_copy(void) {
    fm_pane_t *src = act(), *dst = other();
    if(!require_listing(fm_active) || !require_listing(fm_active^1))return;
    if(!src->count || src->sel<0 || src->sel>=src->count || src->sel>=FM_MAX_ENTRIES)return;

    vfs_dirent_t *e = &src->items[src->sel];
    if (e->is_dir) {
        fm_say(T("Copying whole directories is not supported yet",
                 "Копирование каталогов целиком пока не поддерживается"), 0xFFCC7711);
        return;
    }

    char from[VFS_MAX_PATH], to[VFS_MAX_PATH];
    if(!fm_path(from,src->path,e->name) || !fm_path(to,dst->path,e->name)) {
        fm_say(T("Invalid name or path too long; nothing copied", "Недопустимое имя или длинный путь; копирование отменено"),0xFFCC3344);return;
    }
    if(!strcmp(from,to)) {fm_say(T("Source and target are the same", "Источник и назначение совпадают"),0xFFCC7711);return;}
    u32 epoch=pane_namespace[fm_active];
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
    if (got != (int)size || vfs_generation()!=epoch) {
        kfree(buf);
        fm_say(T("Incomplete read or storage changed; target not written", "Неполное чтение или смена томов; назначение не записано"), 0xFFCC3344);
        return;
    }

    int wrote = vfs_write(to, buf, (u32)got);
    kfree(buf);
    if(vfs_generation()!=epoch) {
        fm_say(T("Storage changed during write; source kept, inspect target", "Смена томов при записи; источник оставлен, проверьте назначение"),0xFFCC3344);return;
    }
    if(wrote>=0 && wrote!=(int)size) {
        fm_say(T("Incomplete write; source kept, inspect target", "Неполная запись; источник оставлен, проверьте назначение"),0xFFCC3344);return;
    }
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
    u32 epoch=vfs_generation();int r=vfs_remove(path);
    if(epoch!=vfs_generation()) {
        fm_refresh();fm_say(T("Delete outcome uncertain; remount and inspect", "Результат удаления неизвестен; перемонтируйте и проверьте"),0xFFCC3344);return;
    }
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
    char full[VFS_MAX_PATH];
    if(!selected_path(full))return;

    if (!vfs_writable(full)) {
        fm_say(T("This volume is read-only", "Этот том только для чтения"), 0xFFCC3344);
        return;
    }
    strncpy(fm_confirm_arg, full, sizeof(fm_confirm_arg));
    fm_confirm_namespace=pane_namespace[fm_active];fm_confirm_listing=listing_epoch;fm_confirm_pane=fm_active;
    fm_confirm = FM_CONFIRM_DELETE;
}

const char *fm_input_title(void) {return fm_input_mode==FM_INPUT_RENAME?T("Rename file", "Переименовать файл"):T("New folder", "Новый каталог");}
const char *fm_input_original(void) {return fm_input_from;}
int fm_input_selected(void) {return fm_input_replace;}
void fm_input_cancel(void) {fm_input_mode=0;fm_say(T("Cancelled", "Отменено"),0);}

static int fm_input_begin(int mode) {
    if(fm_input_mode || fm_confirm || fm_view_open || !require_listing(fm_active))return 0;
    fm_pane_t *p=act();u32 n=0;while(n<sizeof(p->path) && p->path[n])n++;
    if(n==sizeof(p->path)) {fm_say(T("Path is too long", "Путь слишком длинный"),0xFFCC3344);return 0;}
    memcpy(fm_input_dir,p->path,n+1);fm_input_from[0]=fm_input[0]=0;
    fm_input_listing=listing_epoch;fm_input_namespace=pane_namespace[fm_active];fm_input_pane=fm_active;
    fm_input_replace=0;fm_input_mode=mode;fm_say(T("Enter a name; existing files are never replaced", "Введите имя; существующие файлы не заменяются"),0);
    return 1;
}
void fm_ask_rename(void) {
    fm_pane_t *p=act();if(!require_listing(fm_active) || p->sel<0 || p->sel>=p->count || p->sel>=FM_MAX_ENTRIES)return;
    if(p->items[p->sel].is_dir) {fm_say(T("Directory rename is not supported", "Переименование каталогов не поддержано"),0xFFCC7711);return;}
    char from[VFS_MAX_PATH];
    if(!fm_path(from,p->path,p->items[p->sel].name)) {fm_say(T("Invalid source path", "Неверный путь источника"),0xFFCC3344);return;}
    if(!vfs_can_rename(from)) {fm_say(T("Read-only or rename unsupported", "Только чтение или переименование не поддержано"),0xFFCC3344);return;}
    if(!fm_input_begin(FM_INPUT_RENAME))return;
    strcpy(fm_input_from,from);strcpy(fm_input,p->items[p->sel].name);fm_input_replace=1;
}
void fm_input_codepoint(u32 cp) {
    if(!fm_input_mode || cp<32 || cp==127)return;
    char bytes[4];unsigned n=keymap_utf8(cp,bytes);if(!n)return;
    u32 length=fm_input_replace?0:strlen(fm_input);
    if(n>=sizeof(fm_input)-length)return;
    memcpy(fm_input+length,bytes,n);fm_input[length+n]=0;fm_input_replace=0;
}
void fm_input_submit(void) {
    if(!fm_input_mode)return;
    if(fm_input_namespace!=vfs_generation() || fm_input_listing!=listing_epoch || fm_active!=fm_input_pane || strcmp(act()->path,fm_input_dir)) {
        fm_input_mode=0;fm_say(T("Listing changed; reopen the action", "Список изменился; откройте действие заново"),0xFFCC3344);return;
    }
    char full[VFS_MAX_PATH];
    if(native_name_length(fm_input,sizeof(fm_input))<0 || !fm_path(full,fm_input_dir,fm_input)) {
        fm_say(T("Invalid or too long name/path", "Неверное или слишком длинное имя/путь"),0xFFCC3344);return;
    }
    int rename=fm_input_mode==FM_INPUT_RENAME;
    int r=rename?vfs_rename(fm_input_from,full):vfs_mkdir(full);
    if(fm_input_namespace!=vfs_generation() || (rename && r==-6)) {
        fm_input_mode=0;fm_refresh();fm_say(T("Outcome uncertain; remount and inspect; do not retry", "Результат неизвестен; перемонтируйте и проверьте; не повторяйте"),0xFFCC3344);return;
    }
    if(!r) {
        fm_input_mode=0;fm_refresh();
        for(int i=0;i<act()->count;i++)if(!strcmp(act()->items[i].name,fm_input)){act()->sel=i;break;}
        fm_say(rename?T("Renamed", "Переименовано"):T("Directory created", "Каталог создан"),0xFF2E8B57);
    } else if(r==-5)fm_say(T("Name exists; nothing replaced", "Имя существует; замены нет"),0xFFCC7711);
    else if(r==-2)fm_say(T("Read-only or operation unsupported", "Только чтение или операция не поддержана"),0xFFCC3344);
    else fm_say(T("Invalid name or unavailable source", "Неверное имя или недоступный источник"),0xFFCC3344);
}

/* ---------- input ---------- */

int fm_key(int c, int visible_rows) {
    /* All non-text navigation stays inside the modal; Unicode arrives separately. */
    if(fm_input_mode) {
        int len=(int)strlen(fm_input);
        if(c=='\n')fm_input_submit();
        else if(c==27)fm_input_cancel();
        else if(c==1)fm_input_replace=1;
        else if(c==21){fm_input[0]=0;fm_input_replace=0;}
        else if(c=='\b') {
            if(fm_input_replace)len=0;
            else if(len){len--;while(len && ((u8)fm_input[len]&0xc0)==0x80)len--;}
            fm_input[len]=0;fm_input_replace=0;
        } else if(c>=32 && c<127)fm_input_codepoint((u32)c);
        return 1;
    }

    /* waiting for a yes/no answer */
    if (fm_confirm) {
        if (c == 'y' || c == 'Y' || c == '\n') {
            fm_confirm_yes();
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
        case KEY_F7: fm_ask_mkdir(); return 1;
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
int  fm_pane_count(int i)        { return fm_listing_current(i&1)?panes[i&1].count:0; }
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
    if (total_kb) *total_kb = fm_listing_current(i&1)?p->total_kb:0;
    if (free_kb)  *free_kb  = fm_listing_current(i&1)?p->free_kb:0;
}

vfs_dirent_t *fm_pane_item(int i, int index) {
    fm_pane_t *p = &panes[i & 1];
    if(!fm_listing_current(i&1) || index<0 || index>=p->count || index>=FM_MAX_ENTRIES)return NULL;
    return &p->items[index];
}

/* Open whatever is selected: shared by Enter and the double click. */
void fm_activate(void) { fm_enter(); }
void fm_go_up(void)    { fm_updir(); }
void fm_do_copy(void)  { fm_copy(); }
void fm_ask_delete(void) { fm_delete_ask(); }
void fm_ask_mkdir(void) {
    if(!fm_input_begin(FM_INPUT_MKDIR))return;
    if(!vfs_writable(fm_input_dir)) {fm_input_mode=0;fm_say(T("This volume is read-only", "Этот том только для чтения"),0xFFCC3344);}
}
void fm_close_view(void) { fm_view_open = 0; }
void fm_confirm_yes(void) {
    if(fm_confirm==FM_CONFIRM_DELETE) {
        char current[VFS_MAX_PATH];
        if(fm_confirm_namespace!=vfs_generation() || fm_confirm_listing!=listing_epoch || fm_confirm_pane!=fm_active || !selected_path(current) || strcmp(current,fm_confirm_arg))
            fm_say(T("Listing changed; reopen the action", "Список изменился; откройте действие заново"),0xFFCC3344);
        else fm_do_delete(fm_confirm_arg);
    }
    fm_confirm=FM_CONFIRM_NONE;
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
