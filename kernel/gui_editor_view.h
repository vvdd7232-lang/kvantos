/* Private editor geometry and painting, included by gui.c. */
typedef struct { i32 x,y,w,h,tx; int rows,cols,max_top,max_left; } ed_layout_t;
static int ed_clamp(int v,int low,int high) { return v<low?low:v>high?high:v; }
static ed_layout_t ed_layout(window_t *v) {
    ed_layout_t g;
    g.x=v->x+11; g.y=v->y+TITLE_H+1+96; g.w=v->w-22;
    g.rows=(v->y+v->h-g.y-62)/16;
    if(g.rows<1) g.rows=1;
    g.h=g.rows*16+8; g.tx=g.x+(ed_lines>9999?52:36);
    g.cols=(g.w-(g.tx-g.x)-20)/8; if(g.cols<1) g.cols=1;
    g.max_top=ed_lines>g.rows?ed_lines-g.rows:0;
    int longest=editor->doc.width;
    g.max_left=longest>=g.cols?longest-g.cols+1:0;
    editor->view_rows=g.rows; editor->view_cols=g.cols;
    ed_top=ed_clamp(ed_top,0,g.max_top); ed_left=ed_clamp(ed_left,0,g.max_left);
    return g;
}
static void ed_label(i32 x,i32 y,i32 width,const char *s,u32 color) {
    char b[256]; fit_name(b,sizeof(b),s,(u32)(width>8?width/8:1));
    fb_text(x,y,b,color,0xFFFFFFFF);
}
static int ed_text_file(const char *name,u32 size) {
    u32 n=(u32)strlen(name);
    if(n>23 || size>=ED_CAP) return 0;
    if(editor==&dev_editor) return n>2 && !strcmp(name+n-2,".c");
    const char *ext=name;
    for(const char *p=name;*p;p++) if(*p=='.') ext=p;
    return ext==name || !strcmp(ext,".txt") || !strcmp(ext,".c") || !strcmp(ext,".h") ||
           !strcmp(ext,".cfg") || !strcmp(ext,".md") || !strcmp(ext,".log") ||
           !strcmp(ext,".ini") || !strcmp(ext,".csv");
}
static int ed_file(int index,char *name,u32 *size) {
    int shown=0;
    if(editor->file_disk) {
        if(!kvfs_mounted()) return 0;
        char n[44]; u32 sz;
        for(int i=0;i<256;i++) {
            if(kvfs_list(i,n,&sz,NULL)<0) break;
            if(!ed_text_file(n,sz)) continue;
            if(shown++==index) { strncpy(name,n,24); *size=sz; return 1; }
        }
    } else {
        rfile_t *t=ramfs_table();
        for(int i=0;i<RAMFS_MAX_FILES;i++) {
            if(!t[i].used || !ed_text_file(t[i].name,t[i].size)) continue;
            if(shown++==index) { strncpy(name,t[i].name,24); *size=t[i].size; return 1; }
        }
    }
    return 0;
}
#include "gui_editor_import.h"
static void ed_scrollbar(int win,int widget,i32 x,i32 y,i32 length,int vertical,
                         int value,int maximum,int visible) {
    i32 w=vertical?16:length,h=vertical?length:16;
    fb_fill(x,y,w,h,fb_rgb(232,236,243));
    fb_rect(x,y,w,h,fb_rgb(190,199,214));
    fb_text(x+4,y+1,vertical?"^":"<",C_TEXT_DIM,0xFFFFFFFF);
    fb_text(vertical?x+4:x+length-12,vertical?y+length-17:y+1,vertical?"v":">",C_TEXT_DIM,0xFFFFFFFF);
    int track=length-32;
    int knob=maximum?track*visible/(maximum+visible):track;
    knob=ed_clamp(knob,16,track);
    int at=16+(maximum?(track-knob)*value/maximum:0);
    fb_round_fill(vertical?x+2:x+at,vertical?y+at:y+2,
                  vertical?12:knob,vertical?knob:12,maximum?fb_rgb(128,151,187):fb_rgb(197,205,217));
    hit_add(win,widget,x,y,w,h);
}
static void ed_dialog(window_t *v,i32 cy) {
    if(ed_name_mode==ED_IMPORT_DIALOG) { ed_import_view(v,cy); return; }
    int win=(int)(v-wins), file_mode=ed_name_mode<=ED_OPEN_DIALOG;
    i32 x=v->x+25,w=v->w-50,y=cy+(file_mode?8:65);
    i32 h=file_mode?v->h-TITLE_H-18:190;
    hit_remove_window(win); /* Modal: no controls behind the card are clickable. */
    fb_fill(x-4,y-4,w+8,h+8,fb_rgb(215,223,237));
    fb_fill(x,y,w,h,C_WHITE); fb_rect(x,y,w,h,C_ACCENT);
    const char *title=ed_name_mode==ED_OPEN_DIALOG?T("Open a file", "Открыть файл"):
                      ed_name_mode==ED_SAVE_DIALOG?T("Save as", "Сохранить как"):
                      ed_name_mode==ED_DIRTY_DIALOG?T("Save changes?", "Сохранить изменения?"):
                      (ed_name_mode==ED_OVERWRITE_DIALOG || ed_name_mode==ED_IMPORT_REPLACE)?T("Replace existing file?", "Заменить существующий файл?"):
                      T("Delete file from RAM and disk?", "Удалить файл из RAM и с диска?");
    ed_label(x+12,y+12,w-24,title,C_TEXT);
    if(!file_mode) {
        ed_label(x+12,y+46,w-24,(ed_name_mode==ED_OVERWRITE_DIALOG || ed_name_mode==ED_IMPORT_REPLACE)?ed_name_in:ed_name,C_ACCENT);
        ed_label(x+12,y+74,w-24,T("Your document is kept if you cancel.", "При отмене документ останется нетронутым."),C_TEXT_DIM);
        if(ed_status[0]) ed_label(x+12,y+100,w-24,ed_status,C_TEXT_DIM);
        int bw=(w-40)/3;
        widget_chip(win,W_ED_ACCEPT,x+10,y+h-40,bw,28,
                    ed_name_mode==ED_DIRTY_DIALOG?T("Save", "Сохранить"):
                    (ed_name_mode==ED_OVERWRITE_DIALOG || ed_name_mode==ED_IMPORT_REPLACE)?T("Replace", "Заменить"):T("Delete", "Удалить"),1);
        if(ed_name_mode==ED_DIRTY_DIALOG)
            widget_chip(win,W_ED_DISCARD,x+20+bw,y+h-40,bw,28,T("Don't save", "Не сохранять"),0);
        widget_chip(win,W_ED_CANCEL,x+w-bw-10,y+h-40,bw,28,T("Cancel", "Отмена"),0);
        return;
    }
    widget_chip(win,W_ED_RAM,x+12,y+38,120,26,T("RAM (temporary)", "RAM (временно)"),!editor->file_disk);
    widget_chip(win,W_ED_DISK,x+140,y+38,100,26,T("Disk / KvFS", "Диск / KvFS"),editor->file_disk);
    ed_label(x+252,y+43,w-264,ed_name_mode==ED_SAVE_DIALOG?
             (kvfs_mounted()?T("Save to RAM + disk", "Запись в RAM и на диск"):T("No disk: RAM only", "Нет диска: только RAM")):
             T("Select a file below", "Выберите файл ниже"),C_TEXT_DIM);
    i32 ly=y+72,lh=h-178;
    int rows=(lh-8)/20;
    if(rows<1) rows=1;
    fb_fill(x+12,ly,w-54,lh,fb_rgb(248,250,254));
    fb_rect(x+12,ly,w-54,lh,fb_rgb(191,201,217));
    char name[24],info[48]; u32 size;
    int total=0;
    while(ed_file(total,name,&size)) total++;
    editor->file_top=ed_clamp(editor->file_top,0,total>rows?total-rows:0);
    for(int r=0;r<rows && ed_file(editor->file_top+r,name,&size);r++) {
        i32 fy=ly+4+r*20;
        if(!strcmp(name,ed_name_in)) fb_fill(x+14,fy-1,w-58,19,fb_rgb(219,233,254));
        fb_text(x+20,fy,name,C_TEXT,0xFFFFFFFF);
        ksnprintf(info,sizeof(info),"%u B",size);
        fb_text(x+w-146,fy,info,C_TEXT_DIM,0xFFFFFFFF);
        hit_add(win,W_ED_FILE0+editor->file_top+r,x+14,fy-1,w-58,19);
    }
    if(!total) ed_label(x+20,ly+12,w-80,editor->file_disk && !kvfs_mounted()?
                       T("No writable KvFS disk", "Нет доступного диска KvFS"):T("No text files", "Текстовых файлов нет"),C_TEXT_DIM);
    widget_chip(win,W_ED_FUP,x+w-32,ly,22,26,"^",0);
    widget_chip(win,W_ED_FDOWN,x+w-32,ly+lh-26,22,26,"v",0);
    ed_label(x+12,ly+lh+5,w-24,ed_status,C_TEXT_DIM);
    i32 ny=y+h-78;
    fb_text(x+12,ny+5,T("Name:", "Имя:"),C_TEXT_DIM,0xFFFFFFFF);
    fb_fill(x+64,ny,w-76,26,fb_rgb(247,250,255));
    fb_rect(x+64,ny,w-76,26,C_ACCENT);
    fb_text(x+72,ny+5,ed_name_in,C_TEXT,0xFFFFFFFF);
    fb_fill(x+72+editor->name_caret*8,ny+4,2,17,C_ACCENT);
    hit_add(win,W_ED_NAME,x+64,ny,w-76,26);
    widget_chip(win,W_ED_ACCEPT,x+12,y+h-40,140,28,
                ed_name_mode==ED_OPEN_DIALOG?T("Open", "Открыть"):T("Save", "Сохранить"),1);
    widget_chip(win,W_ED_CANCEL,x+w-152,y+h-40,140,28,T("Cancel", "Отмена"),0);
}
static void app_edit(window_t *v,i32 cx,i32 cy,i32 cw) {
    int workshop=v->app==APP_DEV,win=(int)(v-wins);
    editor=workshop?&dev_editor:&note_editor;
    struct { int id; const char *label; } file[]={
        {W_ED_NEW,T("New", "Создать")},{W_ED_OPEN,T("Open", "Открыть")},
        {W_ED_IMPORT,T("Import", "Импорт")},{W_ED_SAVE,T("Save", "Сохранить")},{W_ED_SAVEAS,T("Save as", "Сохр. как")},
        {workshop?W_DEV_HELP:W_ED_DEL,workshop?T("Help", "Справка"):T("Delete", "Удалить")}
    }, edit[]={
        {W_ED_UNDO,T("Undo/redo", "Отмена/повтор")},{W_ED_COPY,T("Copy", "Копия")},
        {W_ED_CUT,T("Cut", "Вырезать")},{W_ED_PASTE,T("Paste", "Вставить")},
        {W_ED_ALL,T("Select all", "Всё")},{W_DEV_BUILD,T("Build", "Собрать")},{W_DEV_RUN,T("Run", "Запуск")}
    };
    int bw=(cw-20)/6;
    for(int i=0;i<6;i++) widget_chip(win,file[i].id,cx+10+i*bw,cy+7,bw-4,25,file[i].label,0);
    int count=workshop?7:5; bw=(cw-20)/count;
    /* A short label also fits a 640x480 desktop. */
    edit[0].label=T("Undo", "Отмена");
    for(int i=0;i<count;i++) widget_chip(win,edit[i].id,cx+10+i*bw,cy+38,bw-4,25,edit[i].label,i>=5);
    char name[192];
    const char *store=kvfs_mounted()?T("RAM + disk", "RAM и диск"):T("RAM: temporary", "RAM: временно");
    ksnprintf(name,sizeof(name),"%s%s  |  %s",ed_name,ed_dirty?" *":"",store);
    if(ed_lines>ED_WARN_LINES)
        ksnprintf(name,sizeof(name),T("%s%s | %d lines (>128): warning only | %s", "%s%s | %d строк (>128): только предупреждение | %s"),ed_name,ed_dirty?" *":"",ed_lines,store);
    ed_label(cx+12,cy+73,cw-24,name,ed_lines>ED_WARN_LINES?fb_rgb(160,100,0):ed_dirty?C_RED:C_TEXT_DIM);
    ed_layout_t g=ed_layout(v);
    fb_fill(g.x,g.y,g.w,g.h,C_WHITE); fb_rect(g.x,g.y,g.w,g.h,fb_rgb(190,201,219));
    fb_fill(g.x+1,g.y+1,g.tx-g.x-4,g.h-2,fb_rgb(244,247,252));
    unsigned a,b; text_range(&editor->doc,&a,&b);
    int caret_on=((u32)timer_ticks()/(timer_hz()/2+1))%2==0;
    for(int i=0;i<g.rows && ed_top+i<ed_lines;i++) {
        int row=ed_top+i; i32 y=g.y+4+i*16;
        char num[8]; ksnprintf(num,sizeof(num),"%d",row+1);
        fb_text(g.x+4,y,num,C_TEXT_DIM,0xFFFFFFFF);
        if(row==ed_cy) fb_fill(g.tx,y-1,g.cols*8,16,fb_rgb(242,247,255));
        unsigned off=text_offset(&editor->doc,row,0);
        int start=text_byte(ed_line(row),ed_left),len=(int)strlen(ed_line(row));
        for(int col=0,pos=start;col<g.cols;col++) {
            int next=text_next(ed_line(row),pos);
            unsigned at=off+(unsigned)pos;
            int selected=a<b && at>=a && at<b && (pos<len || row<ed_lines-1);
            i32 x=g.tx+col*8;
            if(selected) fb_fill(x,y-1,8,16,fb_rgb(54,110,188));
            if(pos<len) {
                char ch[5]; int n=next-pos;
                if(n>4) { ch[0]='?'; ch[1]=0; }
                else { memcpy(ch,ed_line(row)+pos,(u32)n); ch[n]=0; }
                fb_text(x,y,ch,selected?C_WHITE:C_TEXT,0xFFFFFFFF);
                pos=next;
            } else break;
        }
        int col=text_column(ed_line(row),ed_cx)-ed_left;
        if(row==ed_cy && col>=0 && col<g.cols && !ed_name_mode && win==top_window() && caret_on)
            fb_fill(g.tx+col*8,y-1,2,16,C_ACCENT);
    }
    hit_add(win,W_ED_TEXT,g.x,g.y,g.w-18,g.h);
    ed_scrollbar(win,W_ED_VSCROLL,g.x+g.w-17,g.y,g.h,1,ed_top,g.max_top,g.rows);
    ed_scrollbar(win,W_ED_HSCROLL,g.tx,g.y+g.h+2,g.w-(g.tx-g.x)-18,0,ed_left,g.max_left,g.cols);
    i32 sy=v->y+v->h-38;
    if(ed_status[0] && timer_ticks()<ed_status_until) {
        const char *p=ed_status; char part[192]; int n=0,cols=(cw-24)/8;
        for(int line=0;line<2;line++) {
            int count=0; n=0;
            while(*p && count++<cols && n<(int)sizeof(part)-5) {
                const char *begin=p; utf8_next(&p);
                int bytes=(int)(p-begin); memcpy(part+n,begin,(u32)bytes); n+=bytes;
            }
            part[n]=0; fb_text(cx+12,sy+line*16,part,C_ACCENT,0xFFFFFFFF);
        }
    } else {
        ksnprintf(name,sizeof(name),T("Line %d/%d, column %d  |  %s", "Строка %d/%d, столбец %d  |  %s"),
                  ed_cy+1,ed_lines,text_column(ed_line(ed_cy),ed_cx)+1,
                  text_selected(&editor->doc)?T("selection", "выделение"):T("click and drag to select", "выделение мышью"));
        ed_label(cx+12,sy,cw-24,name,C_TEXT_DIM);
        ed_label(cx+12,sy+16,cw-24,T("Click: cursor. Drag: select. Wheel or scrollbars: scroll.",
                                      "Клик: курсор. Протяжка: выделение. Колесо и полосы: прокрутка."),C_TEXT_DIM);
    }
    if(ed_name_mode) ed_dialog(v,cy);
}
static void app_dev(window_t *v,i32 cx,i32 cy,i32 cw) {
    app_edit(v,cx,cy,cw);
    if(!dev_help || ed_name_mode) return;
    int win=(int)(v-wins); hit_remove_window(win);
    i32 x=cx+10,y=cy+8,w=cw-20,h=v->h-TITLE_H-18;
    fb_fill(x,y,w,h,fb_rgb(24,32,50)); fb_rect(x,y,w,h,C_ACCENT);
    const char *lines[]={
        T("C Workshop: edit, build, run", "C-мастерская: редактируйте, собирайте, запускайте"),
        T("Use the New / Open / Save buttons above the text.", "Файлы: кнопки Создать / Открыть / Сохранить."),
        T("Build creates a .kapp; Run builds and starts it.", "Собрать создаёт .kapp; Запуск собирает и запускает."),
        T("RAM is temporary; KvFS files survive reboot.", "RAM временна; файлы на KvFS переживают перезапуск."),
        "void draw(void) { clear(rgb(20,30,50)); }",
        "void click(int x,int y,int b) { /* mouse */ }",
        "void key(int k) { /* keyboard */ }",
        "int, char, pointers; if/else; while; for; functions;",
        "rgb clear fill rect line text status ticks seconds",
        T("Optional: Ctrl+S/C/V/X/A/Z, F5 build, F6 run.", "Необязательно: Ctrl+S/C/V/X/A/Z, F5 сборка, F6 пуск."),
        T("No includes, structs, arrays, casts, floats or libc.", "Нет include, структур, массивов, cast, float и libc."),
        T("Native ring-0 code can hang the OS. Save first!", "Код в ring 0 может повесить ОС. Сначала сохраните!"),
        T("Full reference: sdk/WORKSHOP.md", "Полное описание: sdk/WORKSHOP.ru.md")
    };
    for(unsigned i=0;i<sizeof(lines)/sizeof(lines[0]);i++)
        ed_label(x+12,y+12+(i32)i*21,w-24,lines[i],C_WHITE);
    widget_chip(win,W_DEV_HELP,x+w-158,y+h-40,146,28,T("Close help", "Закрыть справку"),1);
}
