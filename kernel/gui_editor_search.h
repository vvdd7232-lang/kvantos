/* Shared mouse-first, literal UTF-8 find/replace dialog. */
static char *ed_search_text(void) { return editor->search_focus?editor->replacement:editor->query; }
static void ed_search_open(int replace) {
    ed_name_mode=ED_SEARCH_DIALOG; editor->search_focus=replace!=0;
    editor->search_caret=(int)strlen(ed_search_text());
    ed_msg(T("Literal, case-sensitive search. Ctrl+U clears a field.", "Точный поиск с учётом регистра. Ctrl+U очищает поле."));
}
static void ed_find(int backwards) {
    unsigned a,b,at,n=(unsigned)strlen(editor->query);
    if(!n) { ed_msg(T("Enter search text", "Введите текст поиска")); return; }
    text_range(&editor->doc,&a,&b);
    unsigned start=backwards?(a?a-1:editor->doc.used):b;
    if(!text_find(&editor->doc,editor->query,n,start,backwards,1,&at)) {
        ed_msg(T("No match", "Совпадений нет")); return;
    }
    text_position(&editor->doc,at); editor->doc.ay=ed_cy; editor->doc.ax=ed_cx;
    text_position(&editor->doc,at+n); editor->doc.selecting=1;
    ed_scroll_to_cursor();
    int context=editor->view_rows>2?2:0; ed_top=ed_cy>context?ed_cy-context:0;
    ed_msg(T("Match selected; search wraps at document edges", "Совпадение выделено; поиск переходит через край документа"));
}
static void ed_replace_match(int all) {
    unsigned n=(unsigned)strlen(editor->query),m=(unsigned)strlen(editor->replacement),count=0;
    if(!n) { ed_msg(T("Enter search text", "Введите текст поиска")); return; }
    if(all) {
        if(!text_replace_all(&editor->doc,&editor->undo,editor->query,n,editor->replacement,m,&count)) {
            ed_msg(T("Replacement exceeds a text limit; unchanged", "Замена превышает лимит текста; без изменений")); return;
        }
    } else {
        unsigned a,b,found; text_range(&editor->doc,&a,&b);
        if(!text_selected(&editor->doc) || b-a!=n || !text_find(&editor->doc,editor->query,n,a,0,0,&found) || found!=a) {
            ed_find(0); return; /* first select a match, never replace unrelated selection */
        }
        if(!text_replace(&editor->doc,&editor->undo,editor->replacement,m)) {
            ed_msg(T("Replacement exceeds a text limit; unchanged", "Замена превышает лимит текста; без изменений")); return;
        }
        count=1;
    }
    ed_scroll_to_cursor();
    char msg[96]; ksnprintf(msg,sizeof(msg),T("Replaced: %u (one Undo)", "Заменено: %u (одна Отмена)"),count); ed_msg(msg);
}
static void ed_search_key(int c,unsigned cp,unsigned mods,unsigned physical) {
    if(c==27) { ed_name_mode=0; return; }
    if(c=='\n') { ed_find((mods&KMOD_SHIFT)!=0); return; }
    if(c=='\t' || c==6 || (c==8 && (physical=='h' || physical=='H') && (mods&KMOD_CTRL))) {
        editor->search_focus=c=='\t'?!editor->search_focus:c!=6;
        editor->search_caret=(int)strlen(ed_search_text()); return;
    }
    char *s=ed_search_text(); int len=(int)strlen(s),at=editor->search_caret,end=at;
    if(c==1 || c==21) { s[0]=0; editor->search_caret=0; return; }
    if(c==KEY_LEFT) { editor->search_caret=text_prev(s,at); return; }
    if(c==KEY_RIGHT) { editor->search_caret=text_next(s,at); return; }
    if(c==KEY_HOME) { editor->search_caret=0; return; }
    if(c==KEY_END) { editor->search_caret=len; return; }
    if(c=='\b' || c==KEY_DEL) {
        if(c=='\b') at=text_prev(s,at); else end=text_next(s,at);
        for(int i=end;i<=len;i++) s[at+i-end]=s[i];
        editor->search_caret=at; return;
    }
    char encoded[4]; unsigned bytes=cp>=32?keymap_utf8(cp,encoded):0;
    if(bytes) {
        if((unsigned)len+bytes>=ED_QUERY_CAP) { ed_msg(T("Field max 127 UTF-8 bytes", "Поле: до 127 байт UTF-8")); return; }
        for(int i=len;i>=at;i--) s[i+bytes]=s[i];
        for(unsigned i=0;i<bytes;i++) s[at+(int)i]=encoded[i];
        editor->search_caret=at+(int)bytes;
    }
}
static void ed_search_click(int widget) {
    if(widget==W_ED_CANCEL) ed_name_mode=0;
    else if(widget==W_ED_ACCEPT || widget==W_ED_PREVIOUS) ed_find(widget==W_ED_PREVIOUS);
    else if(widget==W_ED_REPLACE || widget==W_ED_REPLACE_ALL) ed_replace_match(widget==W_ED_REPLACE_ALL);
    else if(widget==W_ED_QUERY || widget==W_ED_REPLACEMENT) {
        editor->search_focus=widget==W_ED_REPLACEMENT;
        editor->search_caret=(int)strlen(ed_search_text());
    } else if(widget==W_ED_LAYOUT) { kbd_layout_set(!kbd_layout()); damage_full(); }
}
static void ed_search_field(int win,int id,i32 x,i32 y,i32 w,int which) {
    const char *s=which?editor->replacement:editor->query;
    int active=editor->search_focus==which,cols=(w-16)/8;
    int caret=active?text_column(s,editor->search_caret):0,left=caret>=cols?caret-cols+1:0;
    fb_fill(x,y,w,25,fb_rgb(247,250,255)); fb_rect(x,y,w,25,active?C_ACCENT:C_TEXT_DIM);
    ed_label(x+8,y+5,w-16,s+text_byte(s,left),C_TEXT);
    if(active) fb_fill(x+8+(caret-left)*8,y+4,2,17,C_ACCENT);
    hit_add(win,id,x,y,w,25);
}
static void ed_search_view(window_t *v) {
    int win=(int)(v-wins);
    i32 x=v->x+25,w=v->w-50,h=ED_SEARCH_HEIGHT,y=v->y+v->h-h-48;
    hit_remove_window(win);
    fb_fill(x-4,y-4,w+8,h+8,fb_rgb(215,223,237));
    fb_fill(x,y,w,h,C_WHITE); fb_rect(x,y,w,h,C_ACCENT);
    ed_label(x+12,y+10,w-90,T("Find / replace", "Поиск / замена"),C_TEXT);
    widget_chip(win,W_ED_LAYOUT,x+w-66,y+6,54,24,kbd_layout()?"RU":"EN",0);
    fb_text(x+12,y+42,T("Find:", "Найти:"),C_TEXT_DIM,0xffffffff);
    fb_text(x+12,y+74,T("With:", "Замена:"),C_TEXT_DIM,0xffffffff);
    ed_search_field(win,W_ED_QUERY,x+88,y+36,w-100,0);
    ed_search_field(win,W_ED_REPLACEMENT,x+88,y+68,w-100,1);
    ed_label(x+12,y+100,w-24,ed_status,C_ACCENT);
    int bw=(w-40)/4;
    widget_chip(win,W_ED_PREVIOUS,x+8,y+124,bw,26,T("Previous", "Назад"),0);
    widget_chip(win,W_ED_ACCEPT,x+16+bw,y+124,bw,26,T("Next", "Далее"),1);
    widget_chip(win,W_ED_REPLACE,x+24+2*bw,y+124,bw,26,T("Replace", "Заменить"),0);
    widget_chip(win,W_ED_REPLACE_ALL,x+32+3*bw,y+124,bw,26,T("Replace all", "Заменить всё"),0);
    char context[192]; ksnprintf(context,sizeof(context),T("Line %d: %s", "Строка %d: %s"),ed_cy+1,ed_line(ed_cy));
    ed_label(x+12,y+158,w-24,context,C_TEXT_DIM);
    widget_chip(win,W_ED_CANCEL,x+w-128,y+184,116,26,T("Close", "Закрыть"),0);
}
