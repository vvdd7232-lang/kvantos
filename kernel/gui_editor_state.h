/* Private GUI editor state. Included by gui.c, not a separate translation unit. */
#include "textedit.h"
typedef struct {
    textdoc_t doc;
    textundo_t undo;
    int named, name_mode, pending, file_disk, file_top, name_caret;
    int view_rows, view_cols;
    char name[24], status[192], name_in[24], pending_name[24];
    u64 status_until;
} editor_t;
static editor_t note_editor, dev_editor;
static editor_t *editor = &note_editor;
#define ed_buf          (editor->doc.buf)
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
static int dev_initialized, dev_help;
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
       W_ED_FILE0=1200 };
enum { ED_NEW=1, ED_OPEN, ED_LOAD, ED_CLOSE, ED_QUIT, ED_BUILD, ED_RUN };
enum { ED_SAVE_DIALOG=1, ED_OPEN_DIALOG, ED_DIRTY_DIALOG, ED_OVERWRITE_DIALOG, ED_DELETE_DIALOG };
enum { W_ST_FORMAT = 200, W_ST_REFRESH = 201, W_ST_ITEM0 = 210, W_ST_DEL0 = 300,
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
static void ed_reset(void) {
    text_reset(&editor->doc); editor->undo.valid=0;
    ed_name_mode=editor->pending=editor->file_top=editor->named=0;
    ed_status[0]=0;
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
    editor->undo.valid=0;
    strncpy(ed_name,name,sizeof(ed_name)); editor->named=1;
    return 1;
}
static u32 ed_export(void) { return text_export(&editor->doc,editor_text); }
static int ed_save(void) {
    u32 n=ed_export();
    int rc=ramfs_write(ed_name,editor_text,n);
    if(rc<0) { ed_msg(T("Save failed: RAM full or invalid name", "Не сохранено: RAM заполнена или неверное имя")); return 0; }
    if(kvfs_mounted() && kvfs_write(ed_name,editor_text,n,0)<0) {
        ed_msg(T("RAM saved, disk FAILED. Changes remain unsaved.", "RAM записана, ошибка диска. Документ не сохранён.")); return 0;
    }
    ed_dirty=0; editor->named=1;
    /* Undoing an edit after a save must not mark the older text as saved. */
    if(editor->undo.valid) editor->undo.doc.dirty=1;
    ed_msg(kvfs_mounted()?T("Saved to RAM and disk", "Сохранено в RAM и на диск")
                         :T("RAM only: lost on reboot", "Только RAM: исчезнет при перезапуске"));
    return 1;
}
static void ed_scroll_to_cursor(void) {
    int rows=editor->view_rows?editor->view_rows:16;
    int cols=editor->view_cols?editor->view_cols:60;
    int col=text_column(ed_buf[ed_cy],ed_cx);
    if(ed_cy<ed_top) ed_top=ed_cy;
    if(ed_cy>=ed_top+rows) ed_top=ed_cy-rows+1;
    if(col<ed_left) ed_left=col;
    if(col>=ed_left+cols) ed_left=col-cols+1;
}
static void ed_edit(const char *s,u32 n) {
    if(!text_replace(&editor->doc,&editor->undo,s,n))
        ed_msg(T("Limit: 128 lines, 120 bytes/line. Text unchanged.", "Предел: 128 строк, 120 байт/строка. Текст не изменён."));
    ed_scroll_to_cursor();
}
