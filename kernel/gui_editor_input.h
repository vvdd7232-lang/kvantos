/* Editor actions and pointer capture. Included after dev_build/dev_run. */
static int ed_quit_mask;
static void ed_continue_quit(void);
static void ed_do_pending(int win) {
    int action=editor->pending;
    char name[24]; strncpy(name,editor->pending_name,sizeof(name));
    editor->pending=0; ed_name_mode=0;
    switch(action) {
        case ED_NEW:
            if(editor==&dev_editor) dev_new();
            else { ed_reset(); strncpy(ed_name,"untitled.txt",sizeof(ed_name)); }
            break;
        case ED_OPEN:
            ed_name_mode=ED_OPEN_DIALOG; editor->file_top=0;
            editor->file_disk=kvfs_mounted()?1:0;
            ed_name_in[0]=0; editor->name_caret=0;
            ed_msg(T("Select a file, then click Open", "Выберите файл и нажмите Открыть"));
            break;
        case ED_IMPORT: ed_import_begin(); break;
        case ED_LOAD:
            if(ed_load(name)) ed_msg(T("File opened", "Файл открыт"));
            else { ed_name_mode=ED_OPEN_DIALOG; ed_msg(T("Cannot open: missing, binary or too large", "Не открыть: нет файла, двоичный или превышен лимит")); }
            break;
        case ED_CLOSE: win_close(win); break;
        case ED_QUIT:
            ed_quit_mask|=editor==&dev_editor?2:1;
            ed_continue_quit(); break;
        case ED_BUILD: dev_build(); break;
        case ED_RUN: dev_run(); break;
    }
}
static void ed_request(int win,int action) {
    editor->pending=action;
    if(ed_dirty) {
        ed_name_mode=ED_DIRTY_DIALOG;
        ed_msg(T("Save, discard changes, or cancel this action.", "Сохраните, продолжите без сохранения или отмените."));
    } else ed_do_pending(win);
}
static void ed_continue_quit(void) {
    editor_t *all[]={&note_editor,&dev_editor};
    for(int i=0;i<2;i++) if(all[i]->doc.dirty && !(ed_quit_mask&(1<<i))) {
        int app=i?APP_DEV:APP_EDIT;
        open_app(app); editor=all[i];
        int win=top_window();
        if(win<0 || wins[win].app!=app) {
            set_status(T("Close a window first", "Сначала закройте одно из окон"),C_RED); return;
        }
        ed_request(win,ED_QUIT); return;
    }
    running=0;
}
static void ed_request_quit(void) { ed_quit_mask=0; ed_continue_quit(); }
static void ed_cancel(void) {
    if(ed_name_mode==ED_IMPORT_REPLACE) { ed_name_mode=ED_IMPORT_DIALOG; return; }
    if(ed_name_mode==ED_OVERWRITE_DIALOG) { ed_name_mode=ED_SAVE_DIALOG; return; }
    ed_name_mode=editor->pending=0; ed_quit_mask=0;
    ed_msg(T("Cancelled; document unchanged", "Отменено; документ не изменён"));
}
static int ed_valid_name(void) {
    int n=(int)strlen(ed_name_in);
    if(n<1 || n>23) return 0;
    for(int i=0;i<n;i++) {
        char c=ed_name_in[i];
        if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='.' || c=='_' || c=='-')) return 0;
    }
    return editor!=&dev_editor || (n>=3 && n<=20 && !strcmp(ed_name_in+n-2,".c"));
}
static int ed_exists(const char *name) {
    return ramfs_find(name)!=NULL || (kvfs_mounted() && kvfs_exists(name));
}
static void ed_commit_save(int win) {
    char previous[24]; strncpy(previous,ed_name,sizeof(previous));
    strncpy(ed_name,ed_name_in,sizeof(ed_name));
    if(!ed_save()) { strncpy(ed_name,previous,sizeof(ed_name)); ed_name_mode=ED_SAVE_DIALOG; return; }
    ed_name_mode=0; ed_do_pending(win);
}
static void ed_accept(int win) {
    if(ed_name_mode==ED_IMPORT_DIALOG || ed_name_mode==ED_IMPORT_REPLACE) { ed_import_accept(ed_name_mode==ED_IMPORT_REPLACE); return; }
    if(ed_name_mode==ED_DIRTY_DIALOG) {
        if(editor->named) {
            if(ed_save()) ed_do_pending(win);
        } else ed_prompt_save(T("Choose a name and click Save", "Выберите имя и нажмите Сохранить"));
        return;
    }
    if(ed_name_mode==ED_OVERWRITE_DIALOG) { ed_commit_save(win); return; }
    if(ed_name_mode==ED_DELETE_DIALOG) {
        int had=ed_exists(ed_name);
        if(kvfs_mounted() && kvfs_exists(ed_name) && kvfs_delete(ed_name)<0) {
            ed_msg(T("Disk delete failed; document kept", "Ошибка удаления с диска; документ сохранён в редакторе")); return;
        }
        if(ramfs_find(ed_name)) ramfs_delete(ed_name);
        ed_name_mode=0; editor->named=0; ed_dirty=1;
        ed_msg(had?T("File deleted; buffer kept. Save to recreate.", "Файл удалён; текст оставлен. Сохраните для восстановления."):
                       T("No such file; document kept", "Такого файла нет; текст оставлен"));
        return;
    }
    if(!ed_valid_name()) {
        ed_msg(editor==&dev_editor?T("Use letters/digits/_/-, max 18 + .c", "Имя: буквы/цифры/_/-, до 18 символов и .c"):
                                                   T("Name: 1-23 letters/digits/./_/-", "Имя: 1-23 символа, буквы/цифры/./_/-")); return;
    }
    if(ed_name_mode==ED_OPEN_DIALOG) {
        strncpy(editor->pending_name,ed_name_in,sizeof(editor->pending_name));
        editor->pending=ED_LOAD; ed_do_pending(win); return;
    }
    if(ed_exists(ed_name_in) && (!editor->named || strcmp(ed_name_in,ed_name))) {
        ed_name_mode=ED_OVERWRITE_DIALOG;
        ed_msg(T("The existing file will be replaced only after confirmation.", "Существующий файл будет заменён только после подтверждения."));
        return;
    }
    ed_commit_save(win);
}
static void ed_copy(int cut) {
    unsigned a,b; text_range(&editor->doc,&a,&b);
    if(a==b) { ed_msg(T("Select text with the mouse first", "Сначала выделите текст мышью")); return; }
    ed_export(); memcpy(ed_clipboard,editor_text+a,b-a); ed_clipboard[b-a]=0;
    if(cut) ed_edit("",0);
    ed_msg(cut?T("Cut; Paste works in both editors", "Вырезано; вставка работает в обоих редакторах"):
               T("Copied; Paste works in both editors", "Скопировано; вставка работает в обоих редакторах"));
}
static void ed_build_action(int run) {
    if(!editor->named && ed_exists(ed_name)) {
        editor->pending=run?ED_RUN:ED_BUILD;
        ed_prompt_save(T("Choose a source name before building", "Перед сборкой выберите имя исходника"));
    } else if(run) dev_run(); else dev_build();
}
static void ed_action(int win,int widget) {
    switch(widget) {
        case W_ED_NEW: ed_request(win,ED_NEW); break;
        case W_ED_OPEN: ed_request(win,ED_OPEN); break;
        case W_ED_IMPORT: ed_request(win,ED_IMPORT); break;
        case W_ED_SAVE:
            if(editor->named) ed_save();
            else ed_prompt_save(T("Choose a name and click Save", "Выберите имя и нажмите Сохранить"));
            break;
        case W_ED_SAVEAS: ed_prompt_save(T("Type a new name or select an existing file", "Введите новое имя или выберите существующий файл")); break;
        case W_ED_DEL: ed_name_mode=ED_DELETE_DIALOG; ed_status[0]=0; break;
        case W_ED_UNDO:
            if(text_undo(&editor->doc,&editor->undo)) ed_scroll_to_cursor();
            ed_msg(T("Undo / redo (one step)", "Отмена / повтор (один шаг)")); break;
        case W_ED_COPY: ed_copy(0); break;
        case W_ED_CUT: ed_copy(1); break;
        case W_ED_PASTE:
            if(ed_clipboard[0]) ed_edit(ed_clipboard,(u32)strlen(ed_clipboard));
            else ed_msg(T("Clipboard is empty", "Буфер обмена пуст"));
            break;
        case W_ED_ALL:
            editor->doc.ax=editor->doc.ay=0; editor->doc.selecting=1;
            ed_cy=ed_lines-1; ed_cx=(int)strlen(ed_line(ed_cy)); ed_scroll_to_cursor(); break;
        case W_DEV_BUILD: ed_build_action(0); break;
        case W_DEV_RUN: ed_build_action(1); break;
        case W_DEV_HELP: dev_help=!dev_help; break;
    }
}
static void ed_name_key(int win,int c) {
    if(c==27) { ed_cancel(); return; }
    if(c=='\n') { ed_accept(win); return; }
    if(ed_name_mode>ED_OPEN_DIALOG && ed_name_mode!=ED_IMPORT_DIALOG) return;
    int len=(int)strlen(ed_name_in),at=editor->name_caret;
    if(c==21 || c==1) { ed_name_in[0]=0; editor->name_caret=0; return; }
    if(c==KEY_LEFT) { if(at>0) editor->name_caret--; return; }
    if(c==KEY_RIGHT) { if(at<len) editor->name_caret++; return; }
    if(c==KEY_HOME) { editor->name_caret=0; return; }
    if(c==KEY_END) { editor->name_caret=len; return; }
    if(c=='\b') {
        if(at>0) { for(int i=at-1;i<len;i++) ed_name_in[i]=ed_name_in[i+1]; editor->name_caret--; }
    } else if(c==KEY_DEL) { if(at<len) for(int i=at;i<len;i++) ed_name_in[i]=ed_name_in[i+1]; }
    else if(c>=32 && c<127 && len<23) {
        for(int i=len;i>=at;i--) ed_name_in[i+1]=ed_name_in[i];
        ed_name_in[at]=(char)c; editor->name_caret++;
    }
}
static void ed_key(int win,int c) {
    if(ed_name_mode) { ed_name_key(win,c); return; }
    if(editor==&dev_editor && (dev_help || c==KEY_F1)) {
        if(c==27 || c==KEY_F1) dev_help=!dev_help;
        return;
    }
    switch(c) {
        case 27: editor->doc.selecting=0; return;
        case 19: ed_action(win,W_ED_SAVE); return;
        case 14: ed_action(win,W_ED_NEW); return;
        case 15: case KEY_F2: ed_action(win,W_ED_OPEN); return;
        case 3: ed_copy(0); return;
        case 24: ed_copy(1); return;
        case 22: ed_action(win,W_ED_PASTE); return;
        case 1: ed_action(win,W_ED_ALL); return;
        case 26: case 25: ed_action(win,W_ED_UNDO); return;
        case 23: ed_request(win,ED_CLOSE); return;
        case KEY_F5: if(editor==&dev_editor) ed_build_action(0); return;
        case KEY_F6: if(editor==&dev_editor) ed_build_action(1); return;
        case '\n': ed_edit("\n",1); return;
        case '\t': ed_edit("    ",4); return;
        case '\b': case KEY_DEL:
            if(!text_delete(&editor->doc,&editor->undo,c=='\b'))
                ed_msg(T("Joined line would exceed 120 bytes; unchanged", "Объединённая строка длиннее 120 байт; без изменений"));
            ed_scroll_to_cursor(); return;
    }
    if(c>=KEY_UP && c<=KEY_PGDN) {
        textdoc_t *d=&editor->doc;
        int shifted=kbd_shift();
        if(shifted && !d->selecting) { d->ay=ed_cy; d->ax=ed_cx; d->selecting=1; }
        int col=text_column(ed_line(ed_cy),ed_cx);
        if(!shifted && text_selected(d) && (c==KEY_LEFT || c==KEY_RIGHT)) {
            unsigned a,b; text_range(d,&a,&b); text_position(d,c==KEY_LEFT?a:b);
        } else switch(c) {
            case KEY_LEFT:
                if(ed_cx>0) ed_cx=text_prev(ed_line(ed_cy),ed_cx);
                else if(ed_cy>0) ed_cx=(int)strlen(ed_line(--ed_cy));
                break;
            case KEY_RIGHT:
                if(ed_line(ed_cy)[ed_cx]) ed_cx=text_next(ed_line(ed_cy),ed_cx);
                else if(ed_cy+1<ed_lines) { ed_cy++; ed_cx=0; }
                break;
            case KEY_HOME: ed_cx=0; break;
            case KEY_END: ed_cx=(int)strlen(ed_line(ed_cy)); break;
            case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
                int step=(c==KEY_PGUP || c==KEY_PGDN)?editor->view_rows:1;
                if(c==KEY_UP || c==KEY_PGUP) step=-step;
                ed_cy=ed_clamp(ed_cy+step,0,ed_lines-1); ed_cx=text_byte(ed_line(ed_cy),col); break;
            }
        }
        if(!shifted) d->selecting=0;
        ed_scroll_to_cursor(); return;
    }
    if(c>=32 && c<127) { char ch=(char)c; ed_edit(&ch,1); }
}
static void ed_caret_at(ed_layout_t g,i32 mx,i32 my) {
    int row=ed_clamp((my-g.y-4)/16,0,g.rows-1)+ed_top;
    ed_cy=ed_clamp(row,0,ed_lines-1);
    int col=ed_clamp((mx-g.tx+4)/8,0,g.cols-1)+ed_left;
    ed_cx=text_byte(ed_line(ed_cy),col);
}
static void ed_scroll_press(int win,ed_layout_t g,int vertical,i32 mx,i32 my) {
    int length=vertical?g.h:g.w-(g.tx-g.x)-18,pos=vertical?my-g.y:mx-g.tx;
    int maximum=vertical?g.max_top:g.max_left,view=vertical?g.rows:g.cols;
    int *value=vertical?&ed_top:&ed_left;
    if(!maximum) return;
    int track=length-32,knob=ed_clamp(track*view/(maximum+view),16,track);
    int start=16+(track-knob)*(*value)/maximum;
    if(pos<16) (*value)--;
    else if(pos>=length-16) (*value)++;
    else if(pos>=start && pos<start+knob) {
        ed_drag_win=win; ed_drag_kind=vertical?2:3; ed_drag_grab=pos-start;
    } else *value+=pos<start?-view:view;
    *value=ed_clamp(*value,0,maximum);
}
static void ed_click(int win,i32 mx,i32 my) {
    int widget=hit_test(win,mx,my);
    if(ed_name_mode) {
        if(widget==W_ED_CANCEL) ed_cancel();
        else if(widget==W_ED_ACCEPT) ed_accept(win);
        else if(widget==W_ED_DISCARD && ed_name_mode==ED_DIRTY_DIALOG) ed_do_pending(win);
        else if(ed_name_mode==ED_IMPORT_DIALOG) {
            if(widget==W_ED_VOLUME) ed_import_volume(1);
            else if(widget==W_ED_RESCAN) ed_import_refresh();
            else if(widget==W_ED_PARENT && editor->import_dir[0] && !vfs_is_root(editor->import_dir)) {
                vfs_parent(editor->import_dir,sizeof(editor->import_dir),editor->import_dir); ed_import_list();
            } else if(widget==W_ED_FUP) { if(editor->file_top>0) editor->file_top--; }
            else if(widget==W_ED_FDOWN) editor->file_top++;
            else if(widget==W_ED_NAME) editor->name_caret=ed_clamp((mx-(wins[win].x+25+72)+4)/8,0,(int)strlen(ed_name_in));
            else if(widget>=W_ED_IMPORT0 && widget<W_ED_IMPORT0+128) ed_import_select(widget-W_ED_IMPORT0);
        }
        else if(ed_name_mode<=ED_OPEN_DIALOG) {
            if(widget==W_ED_RAM || widget==W_ED_DISK) { editor->file_disk=widget==W_ED_DISK; editor->file_top=0; }
            else if(widget==W_ED_FUP) { if(editor->file_top>0) editor->file_top--; }
            else if(widget==W_ED_FDOWN) editor->file_top++;
            else if(widget==W_ED_NAME) {
                i32 start=wins[win].x+25+72;
                editor->name_caret=ed_clamp((mx-start+4)/8,0,(int)strlen(ed_name_in));
            } else if(widget>=W_ED_FILE0) {
                u32 size; char name[24];
                if(ed_file(widget-W_ED_FILE0,name,&size)) {
                    strncpy(ed_name_in,name,sizeof(ed_name_in)); editor->name_caret=(int)strlen(ed_name_in);
                }
            }
        }
        return;
    }
    if(editor==&dev_editor && dev_help) { if(widget==W_DEV_HELP) dev_help=0; return; }
    ed_layout_t g=ed_layout(&wins[win]);
    if(widget==W_ED_TEXT) {
        if(!kbd_shift() || !editor->doc.selecting) {
            ed_caret_at(g,mx,my); editor->doc.ay=ed_cy; editor->doc.ax=ed_cx;
        } else ed_caret_at(g,mx,my);
        editor->doc.selecting=1; ed_drag_win=win; ed_drag_kind=1; ed_drag_next=0;
    } else if(widget==W_ED_VSCROLL || widget==W_ED_HSCROLL) ed_scroll_press(win,g,widget==W_ED_VSCROLL,mx,my);
    else ed_action(win,widget);
}
static void ed_drag(i32 mx,i32 my) {
    int win=ed_drag_win;
    if(win<0 || !wins[win].used || wins[win].minimized) { ed_drag_win=-1; return; }
    editor=wins[win].app==APP_DEV?&dev_editor:&note_editor;
    if(ed_name_mode || (editor==&dev_editor && dev_help)) { ed_drag_win=-1; return; }
    ed_layout_t g=ed_layout(&wins[win]);
    int oldtop=ed_top,oldleft=ed_left,oldcy=ed_cy,oldcx=ed_cx;
    if(ed_drag_kind==1) {
        if(timer_ticks()>=ed_drag_next) {
            if(my<g.y+4) ed_top--;
            if(my>=g.y+g.h-4) ed_top++;
            if(mx<g.tx) ed_left--;
            if(mx>=g.tx+g.cols*8) ed_left++;
            ed_top=ed_clamp(ed_top,0,g.max_top); ed_left=ed_clamp(ed_left,0,g.max_left);
            ed_drag_next=timer_ticks()+timer_hz()/20+1;
        }
        ed_caret_at(g,mx,my);
    } else {
        int v=ed_drag_kind==2,length=v?g.h:g.w-(g.tx-g.x)-18,view=v?g.rows:g.cols;
        int maximum=v?g.max_top:g.max_left;
        int track=length-32,knob=ed_clamp(track*view/(maximum+view),16,track);
        int pos=(v?my-g.y:mx-g.tx)-16-ed_drag_grab;
        int value=track>knob?ed_clamp(pos,0,track-knob)*maximum/(track-knob):0;
        if(v) ed_top=value; else ed_left=value;
    }
    if(oldtop!=ed_top || oldleft!=ed_left || oldcy!=ed_cy || oldcx!=ed_cx) damage_window(&wins[win]);
}
static void ed_wheel(i32 mx,i32 my,int delta) {
    for(int z=win_count-1;z>=0;z--) {
        int win=z_order[z]; window_t *v=&wins[win];
        if(!v->used || !v->visible || v->minimized || mx<v->x || mx>=v->x+v->w || my<v->y || my>=v->y+v->h) continue;
        if(v->app!=APP_EDIT && v->app!=APP_DEV) return;
        editor=v->app==APP_DEV?&dev_editor:&note_editor;
        if(ed_name_mode && (ed_name_mode<=ED_OPEN_DIALOG || ed_name_mode==ED_IMPORT_DIALOG)) editor->file_top+=delta*3;
        else if(!ed_name_mode && !(v->app==APP_DEV && dev_help)) {
            ed_layout_t g=ed_layout(v);
            if(kbd_shift()) ed_left=ed_clamp(ed_left+delta*3,0,g.max_left);
            else ed_top=ed_clamp(ed_top+delta*3,0,g.max_top);
        }
        damage_window(v); return;
    }
}
