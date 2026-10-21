/* Mouse-first FAT import. Only reads the source; commits to a local RAM file
 * after complete text validation. Old document/undo survive every failure. */
static textdoc_t import_doc;
static vfs_dirent_t import_batch[32];
static int ed_fat_volume(int i) {
    vfs_volume_t *v=vfs_volume(i);
    return v && (v->kind==FS_FAT16 || v->kind==FS_FAT32);
}
static void ed_import_list(void) {
    editor->import_count=0; editor->import_selected=-1; editor->file_top=0;
    editor->import_file[0]=0;
    if(!ed_destination()) ed_name_in[0]=0;
    editor->name_caret=(int)strlen(ed_destination()?editor->save_name:ed_name_in);
    if(!editor->import_dir[0]) return;
    for(int i=0;i<512;) {
        int count=vfs_readdir_many(editor->import_dir,i,import_batch,32);
        if(count<=0) break;
        i+=count;
        for(int j=0;j<count;j++) {
            vfs_dirent_t entry=import_batch[j];
            if(!strcmp(entry.name,".") || !strcmp(entry.name,"..")) continue;
            int n=(int)strlen(entry.name);
            if(!ed_destination() && editor==&dev_editor && !entry.is_dir &&
               !(n>2 && entry.name[n-2]=='.' && (entry.name[n-1]=='c' || entry.name[n-1]=='C'))) continue;
            if(editor->import_count==128) {
                ed_msg(T("First 128 items shown; use a smaller folder", "Показаны первые 128 файлов; используйте отдельную папку")); return;
            }
            editor->import_items[editor->import_count++]=entry;
        }
        if(count<32) break;
    }
}
static void ed_import_volume(int next) {
    int total=vfs_volume_count(), start=next?editor->import_vol+1:0;
    editor->import_dir[0]=0;
    for(int n=0;n<total;n++) {
        int i=(start+n)%total;
        if(!ed_destination() && !ed_fat_volume(i)) continue;
        editor->import_vol=i;
        ksnprintf(editor->import_dir,sizeof(editor->import_dir),"/mnt/%s",vfs_volume(i)->name);
        break;
    }
    ed_import_list();
    if(ed_destination()) { ed_save_capture(); ed_save_hint(); return; }
    ed_msg(editor->import_dir[0]?
           T("Select a folder/file. Source stays read-only.", "Выберите папку/файл. Запись на источник не производится."):
           (vfs_scan_status()[0]?vfs_scan_status():T("No FAT volume: check USB capture, FAT32 and Refresh.", "Нет FAT-томов. Проверьте захват USB, FAT32 и Обновить.")));
}
static void ed_import_begin(void) {
    ed_name_mode=ED_IMPORT_DIALOG;
    ed_import_volume(0);
}
static void ed_import_refresh(void) {
    /* A global rescan invalidates both editors' old listings. */
    note_editor.import_count=dev_editor.import_count=0;
    note_editor.import_selected=dev_editor.import_selected=-1;
    note_editor.import_dir[0]=dev_editor.import_dir[0]=0;
    note_editor.import_file[0]=dev_editor.import_file[0]=0;
    vfs_init(); vfs_autoscan();
    fm_refresh();
    ed_import_volume(0);
}
static void ed_import_select(int index) {
    if(index<0 || index>=editor->import_count) return;
    vfs_dirent_t *entry=&editor->import_items[index];
    if(strlen(editor->import_dir)+strlen(entry->name)+2>VFS_MAX_PATH) {
        ed_msg(T("Path is too long; unchanged", "Путь слишком длинный; без изменений")); return;
    }
    char path[VFS_MAX_PATH];
    vfs_join(path,sizeof(path),editor->import_dir,entry->name);
    if(entry->is_dir) {
        strncpy(editor->import_dir,path,sizeof(editor->import_dir)); ed_import_list(); return;
    }
    editor->import_selected=index;
    strncpy(editor->import_file,path,sizeof(editor->import_file));
    if(ed_destination()) {
        strncpy(editor->save_name,entry->name,sizeof(editor->save_name));
        editor->name_caret=(int)strlen(editor->save_name); ed_save_hint(); return;
    }
    /* FAT LFN names may be long or Unicode. The local name is separate,
     * editable, and never silently overwrites an existing RAM/KvFS file. */
    int end=(int)strlen(entry->name), dot=end;
    for(int i=0;i<end;i++) if(entry->name[i]=='.') dot=i;
    int n=0;
    for(int i=0;i<dot && n<18;i++) {
        char c=entry->name[i];
        if((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_' || c=='-')
            ed_name_in[n++]=c;
    }
    if(!n) { strncpy(ed_name_in,"import",sizeof(ed_name_in)); n=6; }
    const char *ext=editor==&dev_editor?".c":".txt";
    if(dot<end && end-dot<=4) {
        ext=entry->name+dot;
        for(int i=0;ext[i];i++) if(!((ext[i]>='A'&&ext[i]<='Z') || (ext[i]>='a'&&ext[i]<='z') || ext[i]=='.')) { ext=".txt"; break; }
    }
    if(editor==&dev_editor) ext=".c";
    strncpy(ed_name_in+n,ext,sizeof(ed_name_in)-(u32)n);
    editor->name_caret=(int)strlen(ed_name_in);
    ed_msg(T("Edit the local name if needed, then click Import", "При необходимости измените локальное имя и нажмите Импорт"));
}
static void ed_import_accept(int confirmed) {
    ed_name_mode=ED_IMPORT_DIALOG;
    if(!editor->import_file[0]) { ed_msg(T("Select a file first", "Сначала выберите файл")); return; }
    if(!ed_valid_name()) { ed_msg(T("Invalid local name (ASCII, max 23; workshop: 18 + .c)", "Неверное имя: латиница, до 23; мастерская: 18 и .c")); return; }
    if(!confirmed && ed_exists(ed_name_in)) {
        ed_name_mode=ED_IMPORT_REPLACE;
        ed_msg(T("Only the RAM copy is replaced; disk waits for Save.", "Заменится копия в RAM; запись на диск — только по Сохранить.")); return;
    }
    u32 size=vfs_size(editor->import_file);
    if(vfs_stat(editor->import_file)!=1 || size>=ED_CAP) {
        ed_msg(T("Cannot import: missing/disconnected or too large", "Не импортировать: нет файла/флешки или превышен размер")); ed_name_mode=ED_IMPORT_DIALOG; return;
    }
    int got=vfs_read(editor->import_file,0,editor_text,ED_CAP-1);
    if(got<0 || (u32)got!=size) {
        ed_msg(T("Read failed; document unchanged", "Ошибка чтения; документ не изменён")); ed_name_mode=ED_IMPORT_DIALOG; return;
    }
    unsigned skip=size>=3 && (u8)editor_text[0]==0xEF && (u8)editor_text[1]==0xBB && (u8)editor_text[2]==0xBF ? 3 : 0;
    if((size>=2 && (((u8)editor_text[0]==0xFF && (u8)editor_text[1]==0xFE) ||
                   ((u8)editor_text[0]==0xFE && (u8)editor_text[1]==0xFF))) ||
       !text_set(&import_doc,editor_text+skip,size-skip)) {
        ed_msg(T("Binary/UTF-16, line >120 B or normalized text >64 KiB; unchanged", "Двоичный/UTF-16, строка >120 байт или текст >64 КиБ; без изменений")); ed_name_mode=ED_IMPORT_DIALOG; return;
    }
    u32 n=text_export(&import_doc,editor_text);
    if(ramfs_write(ed_name_in,editor_text,n)<0) {
        ed_msg(T("RAM full; document unchanged", "RAM заполнена; документ не изменён")); ed_name_mode=ED_IMPORT_DIALOG; return;
    }
    ed_storage_changed(ed_name_in,1,0);
    text_copy(&editor->doc,&import_doc);
    strncpy(ed_name,ed_name_in,sizeof(ed_name));
    editor->named=1; ed_dirty=1; ed_name_mode=editor->pending=0; editor->target[0]=0;
    text_history_reset(&editor->doc,&editor->undo);
    editor->ram_valid=editor->imported=1; editor->ram_revision=editor->undo.current;
    editor->disk_valid=editor->disk_failed=0;
    ed_msg(kvfs_mounted()?T("Imported to RAM. Click Keep: KvFS to persist. USB unchanged.", "Импорт в RAM. Нажмите На KvFS для сохранения. USB не изменён."):
        T("Imported to RAM only: no KvFS disk. USB unchanged.", "Импорт только в RAM: нет диска KvFS. USB не изменён."));
}
static void ed_import_view(window_t *v,i32 cy) {
    int win=(int)(v-wins),saving=ed_destination();
    i32 x=v->x+25,w=v->w-50,y=cy+8,h=v->h-TITLE_H-18;
    hit_remove_window(win);
    fb_fill(x-4,y-4,w+8,h+8,C_FIELD_BORDER);
    fb_fill(x,y,w,h,C_WIN); fb_rect(x,y,w,h,C_ACCENT);
    ed_label(x+12,y+12,w-24,saving?T("Save as: choose volume and folder", "Сохранить как: том и папка"):T("Import from FAT / USB (read-only)", "Импорт с FAT / USB (только чтение)"),C_TEXT);
    widget_chip(win,W_ED_VOLUME,x+12,y+38,110,26,T("Next volume", "Носитель >"),0);
    widget_chip(win,W_ED_RESCAN,x+130,y+38,100,26,T("Refresh USB", "Обновить"),0);
    widget_chip(win,W_ED_PARENT,x+238,y+38,78,26,T("Up", "Вверх"),0);
    char location[64]; vfs_volume_t *vol=vfs_volume(editor->import_vol);
    ksnprintf(location,sizeof(location),"%s %s",vol?vfs_kind_name(vol->kind):"-",vfs_writable(editor->import_dir)?"RW":"RO");
    ed_label(x+326,y+43,w-338,saving?location:usb_status(),saving && !vfs_writable(editor->import_dir)?C_RED:C_TEXT_DIM);
    ed_label(x+12,y+73,w-24,editor->import_dir[0]?editor->import_dir:(saving?T("No mounted volumes", "Нет подключённых томов"):T("No FAT16/FAT32 volumes", "Нет томов FAT16/FAT32")),C_ACCENT);
    i32 ly=y+96,lh=h-202;
    int rows=(lh-8)/20;
    if(rows<1) rows=1;
    editor->file_top=ed_clamp(editor->file_top,0,editor->import_count>rows?editor->import_count-rows:0);
    fb_fill(x+12,ly,w-54,lh,C_FIELD); fb_rect(x+12,ly,w-54,lh,C_FIELD_BORDER);
    for(int r=0;r<rows && editor->file_top+r<editor->import_count;r++) {
        int index=editor->file_top+r; vfs_dirent_t *entry=&editor->import_items[index];
        i32 fy=ly+4+r*20; char info[32];
        if(index==editor->import_selected) fb_fill(x+14,fy-1,w-58,19,C_SELECT);
        ed_label(x+20,fy,w-178,entry->name,C_TEXT);
        if(entry->is_dir) strncpy(info,T("<folder>", "<папка>"),sizeof(info));
        else ksnprintf(info,sizeof(info),"%u B",entry->size);
        ed_label(x+w-148,fy,94,info,C_TEXT_DIM);
        hit_add(win,W_ED_IMPORT0+index,x+14,fy-1,w-58,19);
    }
    if(!editor->import_count) ed_label(x+20,ly+12,w-80,(saving?T("Empty folder: enter a new file name", "Папка пуста: введите имя нового файла"):T("No files; check controller, format and Refresh", "Нет файлов; проверьте контроллер, FAT32 и Обновить")),C_TEXT_DIM);
    widget_chip(win,W_ED_FUP,x+w-32,ly,22,26,"^",0);
    widget_chip(win,W_ED_FDOWN,x+w-32,ly+lh-26,22,26,"v",0);
    ed_label(x+12,ly+lh+5,w-24,ed_status,C_TEXT_DIM);
    i32 ny=y+h-78;
    fb_text(x+12,ny+5,T("Name:", "Имя:"),C_TEXT_DIM,0xFFFFFFFF);
    fb_fill(x+64,ny,w-76,26,C_FIELD); fb_rect(x+64,ny,w-76,26,C_ACCENT);
    const char *name=saving?editor->save_name:ed_name_in;
    int col=text_column(name,editor->name_caret),cols=(w-92)/8,left=col>=cols?col-cols+1:0;
    ed_label(x+72,ny+5,w-92,name+text_byte(name,left),C_TEXT);
    fb_fill(x+72+(col-left)*8,ny+4,2,17,C_ACCENT);
    hit_add(win,W_ED_NAME,x+64,ny,w-76,26);
    widget_chip(win,W_ED_ACCEPT,x+12,y+h-40,140,28,saving?T("Save", "Сохранить"):T("Import", "Импорт"),1);
    widget_chip(win,W_ED_CANCEL,x+w-152,y+h-40,140,28,T("Cancel", "Отмена"),0);
}
