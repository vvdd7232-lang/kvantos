/* Both editors share the VFS destination picker with the read-only importer.
 * Legacy Save still keeps RAM+KvFS; explicit destinations write ONLY that file.
 * Never silently rebind a path after rescan or a transport generation change. */
static vfs_volume_t *ed_path_volume(const char *path) {
    char name[16]; unsigned n=0;
    if(strncmp(path,"/mnt/",5)) return NULL;
    path+=5;
    while(*path && *path!='/' && n<sizeof(name)-1) name[n++]=*path++;
    name[n]=0;
    return *path && *path!='/'?NULL:vfs_find(name);
}
static u32 ed_volume_usb(vfs_volume_t *v) {
    return v?blk_generation(v->disk):0;
}
static int ed_target_live(const char *path,u32 epoch,u32 usb) {
    vfs_volume_t *v=ed_path_volume(path);
    return epoch==vfs_generation() && v && ed_volume_usb(v)==usb && vfs_writable(path);
}
static void ed_save_capture(void) {
    editor->save_epoch=vfs_generation();
    editor->save_usb=ed_volume_usb(ed_path_volume(editor->import_dir));
}
static void ed_save_hint(void) {
    vfs_volume_t *v=ed_path_volume(editor->import_dir);
    ed_msg(!v?T("No volume. Refresh and choose a destination.", "Нет тома. Обновите список и выберите назначение."):
        !vfs_writable(editor->import_dir)?T("RO: this volume is read-only. Choose another volume.", "RO: том только для чтения. Выберите другой том."):
        v->kind==FS_RAMFS?T("RAM only: lost on reboot. Save writes only this destination.", "Только RAM: исчезнет при перезапуске. Запись только в выбранное место."):
        T("RW: choose a folder/name. Existing files require confirmation.", "RW: выберите папку и имя. Замена файла — с подтверждением."));
}
static void ed_dest_begin(void) {
    const char *name=ed_name;
    if(editor->target[0]) {
        name=editor->target;
        for(const char *p=name;*p;p++) if(*p=='/') name=p+1;
    } else if(ed_name_mode==ED_SAVE_DIALOG && ed_name_in[0]) name=ed_name_in;
    strncpy(editor->save_name,name,sizeof(editor->save_name));
    ed_name_mode=ED_DEST_DIALOG;
    ed_import_volume(0);
    if(editor->target[0] && ed_target_live(editor->target,editor->target_epoch,editor->target_usb)) {
        vfs_volume_t *vol=ed_path_volume(editor->target);
        for(int i=0;i<vfs_volume_count();i++) if(vfs_volume(i)==vol) editor->import_vol=i;
        vfs_parent(editor->import_dir,sizeof(editor->import_dir),editor->target);
        ed_import_list(); ed_save_capture(); ed_save_hint();
    }
}
static void ed_target_changed(const char *path) {
    editor_t *all[]={&note_editor,&dev_editor};
    vfs_volume_t *v=ed_path_volume(path);
    for(int i=0;i<2;i++) {
        editor_t *other=all[i];
        /* FAT has case and short-name aliases. Conservatively invalidate the
           other bound buffer on this volume rather than miss an alias. */
        int fat_alias=v && (v->kind==FS_FAT16 || v->kind==FS_FAT32) && ed_path_volume(other->target)==v;
        if(other==editor || !other->target[0] || (strcmp(other->target,path) && !fat_alias)) continue;
        other->target_valid=0; other->undo.saved=~0u; other->doc.dirty=1;
    }
}
static int ed_save_target(const char *path,u32 epoch,u32 usb) {
    if(!ed_target_live(path,epoch,usb)) {
        ed_msg(T("Not saved: RO, disconnected or rescanned. Choose destination again.", "Не сохранено: RO, отключение или пересканирование. Выберите назначение заново."));
        if(!strcmp(path,editor->target)) editor->target_failed=1;
        return 0;
    }
    vfs_volume_t *v=ed_path_volume(path);
    if(vfs_stat(path)==2) { ed_msg(T("A folder cannot be overwritten", "Нельзя заменить папку файлом")); return 0; }
    u32 n=ed_export();
    if(vfs_write(path,editor_text,n)!=(int)n) {
        ed_msg(T("WRITE FAILED: text, Undo and previous destination kept. Check media/space.", "ОШИБКА ЗАПИСИ: текст, история и прежнее назначение оставлены. Проверьте носитель и место."));
        if(!strcmp(path,editor->target)) editor->target_failed=1;
        return 0;
    }
    const char *name=path;
    for(const char *p=path;*p;p++) if(*p=='/') name=p+1;
    if(v->kind==FS_RAMFS || v->kind==FS_KVFS) {
        ed_storage_changed(name,v->kind==FS_RAMFS,v->kind==FS_KVFS);
        /* Long KvFS names do not rename the short local build alias. */
        u32 len=(u32)strlen(name);
        int alias=len<sizeof(ed_name) && (editor!=&dev_editor ||
            (len>=3 && len<=20 && !strcmp(name+len-2,".c")));
        if(alias) {
            if(strcmp(name,ed_name)) editor->ram_valid=editor->disk_valid=0;
            strncpy(ed_name,name,sizeof(ed_name));
            if(v->kind==FS_RAMFS) { editor->ram_valid=1;editor->ram_revision=editor->undo.current; }
            else { editor->disk_valid=1;editor->disk_revision=editor->undo.current;editor->disk_failed=0; }
        }
    }
    ed_target_changed(path);
    strncpy(editor->target,path,sizeof(editor->target));
    editor->target_epoch=epoch; editor->target_usb=usb;
    editor->target_valid=1; editor->target_failed=0; editor->target_revision=editor->undo.current;
    editor->named=1;
    if(v->kind!=FS_RAMFS) editor->imported=0;
    text_saved(&editor->doc,&editor->undo);
    ed_msg(v->kind==FS_RAMFS?T("Saved ONLY in RAM: lost on reboot", "Сохранено ТОЛЬКО в RAM: исчезнет при перезапуске"):
        T("Saved to selected VFS destination; other volumes unchanged", "Сохранено в выбранное место VFS; остальные тома не изменены"));
    return 1;
}
static int ed_dest_name(vfs_volume_t *v) {
    const char *s=editor->save_name; unsigned n=(unsigned)strlen(s);
    unsigned max=v->kind==FS_RAMFS?23:v->kind==FS_KVFS?39:127;
    if(!n || n>max || !strcmp(s,".") || !strcmp(s,"..") || s[n-1]=='.' || s[n-1]==' ') return 0;
    for(unsigned i=0;i<n;i++) {
        u8 c=(u8)s[i];
        if(c<32 || c==127 || c=='/' || c=='\\' || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|' || c>=0xF0) return 0;
        if((v->kind==FS_RAMFS || v->kind==FS_KVFS) && c>=128) return 0;
    }
    return editor!=&dev_editor || (n>=3 && s[n-2]=='.' && (s[n-1]=='c' || s[n-1]=='C'));
}
static int ed_dest_accept(int confirmed) {
    vfs_volume_t *v=ed_path_volume(editor->import_dir);
    if(!v || !ed_target_live(editor->import_dir,editor->save_epoch,editor->save_usb)) {
        ed_name_mode=ED_DEST_DIALOG;ed_msg(T("Not saved: RO or device changed. Refresh and choose again.", "Не сохранено: RO или смена устройства. Обновите список и выберите заново."));return 0;
    }
    if(!ed_dest_name(v)) {
        ed_name_mode=ED_DEST_DIALOG;
        ed_msg(T("Invalid name/length; local names ASCII, workshop requires .c", "Неверное имя/длина; локальные имена — латиница, мастерская требует .c"));return 0;
    }
    char path[VFS_MAX_PATH];
    if(strlen(editor->import_dir)+strlen(editor->save_name)+2>sizeof(path) || vfs_stat(editor->import_dir)!=2) {
        ed_name_mode=ED_DEST_DIALOG;ed_msg(T("Folder missing or path too long; unchanged", "Нет папки или путь слишком длинный; без изменений"));return 0;
    }
    vfs_join(path,sizeof(path),editor->import_dir,editor->save_name);
    if(confirmed && strcmp(path,editor->import_file)) { ed_name_mode=ED_DEST_DIALOG;return 0; }
    int exists=vfs_stat(path);
    if(exists==2) { ed_name_mode=ED_DEST_DIALOG;ed_msg(T("Choose another name: a folder already exists", "Выберите другое имя: уже существует папка"));return 0; }
    if(exists==1 && !confirmed) {
        strncpy(editor->import_file,path,sizeof(editor->import_file));
        ed_name_mode=ED_DEST_REPLACE;
        ed_msg(T("Replace only this file? No other volume will be written.", "Заменить только этот файл? Другие тома не затрагиваются."));return 0;
    }
    if(!ed_save_target(path,editor->save_epoch,editor->save_usb)) { ed_name_mode=ED_DEST_DIALOG;return 0; }
    ed_name_mode=0;return 1;
}
static void ed_dest_key(int c,unsigned cp) {
    /* Local files retain physical ASCII naming, including under RU layout.
       FAT names use UTF-8 and support the same BMP repertoire as the driver. */
    vfs_volume_t *v=ed_path_volume(editor->import_dir);
    char *s=editor->save_name;int len=(int)strlen(s),at=editor->name_caret,end=at;
    if(c==27) { ed_name_mode=editor->pending=0;return; }
    if(c=='\n') return; /* Routed through ed_accept, with pending-action guard. */
    if(c==1 || c==21) { s[0]=0;editor->name_caret=0;return; }
    if(c==KEY_LEFT) { editor->name_caret=text_prev(s,at);return; }
    if(c==KEY_RIGHT) { editor->name_caret=text_next(s,at);return; }
    if(c==KEY_HOME) { editor->name_caret=0;return; }
    if(c==KEY_END) { editor->name_caret=len;return; }
    if(c=='\b' || c==KEY_DEL) {
        if(c=='\b') at=text_prev(s,at);else end=text_next(s,at);
        for(int i=end;i<=len;i++) s[at+i-end]=s[i];
        editor->name_caret=at;return;
    }
    if(v && (v->kind==FS_RAMFS || v->kind==FS_KVFS)) cp=c>=32 && c<127?(unsigned)c:0;
    char bytes[4];unsigned n=cp>=32?keymap_utf8(cp,bytes):0;
    if(n && len+n<sizeof(editor->save_name)) {
        for(int i=len;i>=at;i--) s[i+n]=s[i];
        memcpy(s+at,bytes,n);editor->name_caret=at+(int)n;
    }
}
