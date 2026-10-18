/* Private GUI editor state. Included by gui.c, not a separate translation unit. */
#include "textedit.h"
typedef struct {
    textdoc_t doc;
    textundo_t undo;
    int named, name_mode, pending, file_disk, file_top, name_caret;
    int view_rows, view_cols;
    char name[24], status[192], name_in[24], pending_name[24];
    u64 status_until;
    char import_dir[VFS_MAX_PATH], import_file[VFS_MAX_PATH];
    vfs_dirent_t import_items[128]; /* Cached only on navigation, not redraw. */
    int import_count, import_selected, import_vol;
    char query[ED_QUERY_CAP], replacement[ED_QUERY_CAP];
    int search_focus, search_caret;
    u32 ram_revision, disk_revision;
    int ram_valid, disk_valid, disk_failed, imported;
    char target[VFS_MAX_PATH], save_name[VFS_MAX_NAME];
    u32 target_epoch, target_usb, target_revision, save_epoch, save_usb;
    int target_valid, target_failed;

} editor_t;
static editor_t note_editor, dev_editor;
static editor_t *editor = &note_editor;
#define ed_line(r)      text_line(&editor->doc,(r))
#define ed_lines        (editor->doc.lines)
#define ed_cy           (editor->doc.cy)
#define ed_cx           (editor->doc.cx)
#define ed_top          (editor->doc.top)
#define ed_left         (editor->doc.left)
#define ed_dirty        (editor->doc.dirty)
#define ed_name         (editor->name)
#define ed_status       (editor->status)
#define ed_status_until (editor->status_until)
#define ed_name_mode    (editor->name_mode)
#define ed_name_in      (editor->name_in)
static char editor_text[ED_CAP], ed_clipboard[ED_CAP];
static int dev_initialized, dev_help, dev_isolated;
static int ed_drag_win = -1, ed_drag_kind, ed_drag_grab;
static u64 ed_drag_next;
static void ed_request_quit(void);
static void dev_new(void);
static int dev_build(void);
static void dev_run(void);
static void widget_chip(int win, int id, i32 x, i32 y, i32 w, i32 h,
                        const char *label, int active);
enum { W_ED_NEW=40, W_ED_OPEN, W_ED_SAVE, W_ED_DEL,
       W_ED_SAVEAS=76, W_ED_UNDO, W_ED_COPY, W_ED_CUT, W_ED_PASTE, W_ED_ALL,
       W_ED_ACCEPT, W_ED_CANCEL, W_ED_DISCARD, W_ED_FUP, W_ED_FDOWN,
       W_ED_VSCROLL, W_ED_HSCROLL, W_ED_TEXT,
       W_DEV_BUILD, W_DEV_RUN, W_DEV_HELP, W_ED_NAME, W_ED_RAM, W_ED_DISK,
       W_ED_IMPORT=96, W_ED_RESCAN, W_ED_PARENT, W_ED_VOLUME, W_ED_REDO, W_ED_LAYOUT, W_ED_FIND,
       W_ED_PREVIOUS=104, W_ED_REPLACE, W_ED_REPLACE_ALL, W_ED_QUERY, W_ED_REPLACEMENT, W_ED_KEEP,
       W_DEV_ISOLATE=110, W_ED_IMPORT0=2000, W_ED_FILE0=1200 };
enum { ED_NEW=1, ED_OPEN, ED_LOAD, ED_CLOSE, ED_QUIT, ED_BUILD, ED_RUN, ED_IMPORT };
enum { ED_SAVE_DIALOG=1, ED_OPEN_DIALOG, ED_DIRTY_DIALOG, ED_OVERWRITE_DIALOG, ED_DELETE_DIALOG, ED_IMPORT_DIALOG, ED_IMPORT_REPLACE, ED_SEARCH_DIALOG, ED_DEST_DIALOG, ED_DEST_REPLACE };
enum { W_ST_FORMAT = 200, W_ST_REFRESH = 201, W_ST_FORMAT_YES=202, W_ST_FORMAT_NO=203, W_ST_ITEM0 = 210, W_ST_DEL0 = 300,
       W_ST_RAM0 = 400, W_ST_SETUP = 500, W_ST_SETUP_KEEP = 501 };

static void ed_msg(const char *m) {
    strncpy(ed_status,m,sizeof(ed_status));
    ed_status_until=timer_ticks()+timer_hz()*15;
}
static void ed_prompt_save(const char *message) {
    ed_name_mode=ED_SAVE_DIALOG;
    strncpy(ed_name_in,ed_name,sizeof(ed_name_in));
    editor->name_caret=(int)strlen(ed_name_in);
    ed_msg(message);
}
static int ed_save_target(const char *path,u32 epoch,u32 usb);
static int ed_target_live(const char *path,u32 epoch,u32 usb);
static void ed_save_capture(void);
static void ed_target_changed(const char *path);
static void ed_save_hint(void);
static int ed_destination(void) { return ed_name_mode==ED_DEST_DIALOG; }
static void ed_storage_changed(const char *name,int ram,int disk) {
    char path[VFS_MAX_PATH];
    if(ram) { ksnprintf(path,sizeof(path),"/mnt/ram/%s",name);ed_target_changed(path); }
    if(disk) { ksnprintf(path,sizeof(path),"/mnt/kv/%s",name);ed_target_changed(path); }
    editor_t *all[]={&note_editor,&dev_editor};
    for(int i=0;i<2;i++) {
        editor_t *other=all[i];
        if(other==editor || strcmp(other->name,name)) continue;
        if(ram) other->ram_valid=0;
        if(disk) other->disk_valid=0;
        if(!(other->ram_valid && other->ram_revision==other->undo.current) &&
           !(other->disk_valid && other->disk_revision==other->undo.current)) {
            other->undo.saved=~0u; other->doc.dirty=1;
        }
    }
}
static void ed_storage_status(char *out,u32 capacity) {
    if(editor->target[0]) {
        int live=ed_target_live(editor->target,editor->target_epoch,editor->target_usb);
        ksnprintf(out,capacity,"%s | %s | %s",
            !live?T("UNAVAILABLE / RO", "НЕДОСТУПЕН / RO"):editor->target_failed?T("WRITE FAILED", "ОШИБКА ЗАПИСИ"):
            editor->target_valid && editor->target_revision==editor->undo.current?T("saved", "сохранено"):T("not saved", "не сохранено"),
            !strncmp(editor->target,"/mnt/ram/",9)?T("RAM temporary", "RAM временно"):T("Destination", "Назначение"),editor->target);
        return;
    }
    int ram=editor->ram_valid && editor->ram_revision==editor->undo.current;
    int disk=editor->disk_valid && editor->disk_revision==editor->undo.current;
    ksnprintf(out,capacity,"%s | RAM: %s | KvFS: %s",
        ed_dirty?T("Modified", "Изменено"):T("Unmodified", "Без изменений"),
        ram?T("saved", "сохранено"):T("not saved", "не сохранено"),
        editor->disk_failed?T("WRITE FAILED", "ОШИБКА ЗАПИСИ"):
        !kvfs_mounted()?T("no disk", "нет диска"):disk?T("saved", "сохранено"):T("not saved", "не сохранено"));
}
static void ed_reset(void) {
    text_reset(&editor->doc); text_history_reset(&editor->doc,&editor->undo);
    ed_name_mode=editor->pending=editor->file_top=editor->named=0;
    ed_status[0]=editor->target[0]=0;
    editor->ram_valid=editor->disk_valid=editor->disk_failed=editor->imported=0;
}
static int ed_load(const char *name) {
    rfile_t *f=editor->file_disk?NULL:ramfs_find(name);
    const char *s; u32 n;
    if(f) { s=f->data; n=f->size; }
    else {
        if(!kvfs_mounted() || !kvfs_exists(name)) return 0;
        n=kvfs_size(name);
        if(n>=sizeof(editor_text)) return 0;
        int got=kvfs_read(name,editor_text,sizeof(editor_text)-1);
        if(got<0 || (u32)got!=n) return 0;
        s=editor_text;
    }
    if(!text_set(&editor->doc,s,n)) return 0;
    text_history_reset(&editor->doc,&editor->undo);
    editor->target[0]=0;
    strncpy(ed_name,name,sizeof(ed_name)); editor->named=1;
    rfile_t *ram=ramfs_find(name);
    editor->ram_valid=ram && ram->size==n && !memcmp(ram->data,s,n);
    editor->disk_valid=!f;
    editor->ram_revision=editor->disk_revision=editor->undo.current;
    editor->disk_failed=editor->imported=0;
    return 1;
}
static u32 ed_export(void) { return text_export(&editor->doc,editor_text); }
static int ed_save_local(void) {
    u32 n=ed_export();
    int rc=ramfs_write(ed_name,editor_text,n);
    if(rc<0) { ed_msg(T("Save failed: RAM full or invalid name", "Не сохранено: RAM заполнена или неверное имя")); return 0; }
    editor->ram_valid=1; editor->ram_revision=editor->undo.current;
    ed_storage_changed(ed_name,1,0);
    if(kvfs_mounted() && kvfs_write(ed_name,editor_text,n,0)<0) {
        editor->disk_valid=0; editor->disk_failed=1;
        ed_storage_changed(ed_name,0,1);
        ed_msg(T("RAM saved, disk FAILED. Changes remain unsaved.", "RAM записана, ошибка диска. Документ не сохранён.")); return 0;
    }
    editor->disk_valid=kvfs_mounted(); editor->disk_revision=editor->undo.current; editor->disk_failed=0;
    if(editor->disk_valid) { editor->imported=0; ed_storage_changed(ed_name,0,1); }
    text_saved(&editor->doc,&editor->undo); editor->named=1; editor->target[0]=0;
    ed_msg(kvfs_mounted()?T("Saved to RAM and disk", "Сохранено в RAM и на диск")
                         :T("RAM only: lost on reboot", "Только RAM: исчезнет при перезапуске"));
    return 1;
}
static int ed_save(void) {
    return editor->target[0]?ed_save_target(editor->target,editor->target_epoch,editor->target_usb):ed_save_local();
}
static void ed_scroll_to_cursor(void) {
    int rows=editor->view_rows?editor->view_rows:16;
    int cols=editor->view_cols?editor->view_cols:60;
    int col=text_column(ed_line(ed_cy),ed_cx);
    if(ed_cy<ed_top) ed_top=ed_cy;
    if(ed_cy>=ed_top+rows) ed_top=ed_cy-rows+1;
    if(col<ed_left) ed_left=col;
    if(col>=ed_left+cols) ed_left=col-cols+1;
}
static void ed_edit(const char *s,u32 n) {
    if(!text_replace(&editor->doc,&editor->undo,s,n))
        ed_msg(T("Limit: 64 KiB or 120 bytes/line. Text unchanged.", "Предел: 64 КиБ или 120 байт/строка. Текст не изменён."));
    ed_scroll_to_cursor();
}

static int ed_valid_name(void);
static int ed_exists(const char *name);
