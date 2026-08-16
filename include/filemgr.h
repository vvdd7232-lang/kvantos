/* ============================================================
 *  KvantOS - file manager: the interface gui.c draws through
 * ============================================================ */
#ifndef KV_FILEMGR_H
#define KV_FILEMGR_H

#include "vfs.h"

/* A copy goes through one heap buffer, so it is bounded. Anything
   larger needs a streaming copy, which the manager does not do yet. */
#define FM_COPY_LIMIT   (4u * 1024 * 1024)

enum { FM_CONFIRM_NONE = 0, FM_CONFIRM_DELETE };

void fm_init(void);
void fm_refresh(void);
int  fm_key(int c, int visible_rows);

/* actions, shared by the keyboard and the toolbar buttons */
void fm_activate(void);
void fm_go_up(void);
void fm_do_copy(void);
void fm_ask_delete(void);
void fm_ask_mkdir(void);
void fm_next_volume(void);
void fm_close_view(void);
void fm_confirm_yes(void);
void fm_confirm_no(void);
void fm_say(const char *msg, u32 colour);

/* state for the drawing code */
int          fm_active_pane(void);
void         fm_set_active(int i);
const char  *fm_pane_path(int i);
int          fm_pane_count(int i);
int          fm_pane_sel(int i);
int          fm_pane_scroll(int i);
void         fm_pane_set_sel(int i, int sel);
void         fm_pane_scroll_by(int i, int delta, int visible_rows);
vfs_dirent_t *fm_pane_item(int i, int index);
void         fm_pane_space(int i, u32 *total_kb, u32 *free_kb);

const char  *fm_status_text(void);
u32          fm_status_colour(void);

int          fm_view_is_open(void);
const char  *fm_view_title(void);
const char  *fm_view_data(void);
int          fm_view_length(void);
int          fm_view_is_binary(void);
int          fm_view_scroll_pos(void);
void         fm_view_scroll_by(int d);

int          fm_confirm_pending(void);
const char  *fm_confirm_target(void);
int          fm_input_active(void);
const char  *fm_input_text(void);

#endif
