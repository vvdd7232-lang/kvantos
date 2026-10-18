/* Real file-manager/VFS modal logic; no kernel privilege or disk accesses. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/filemgr.c"
#include "../../kernel/vfs.c"
kv_lang_t kv_current_lang=KV_LANG_EN;
static int renames,mkdirs,result,published,epoch,allocations;
static int reads,removes,list_calls,list_failure,list_epoch,list_return,bad_name,space_epoch,read_epoch,remove_epoch,read_result;
static int batch_result,batch_epoch;
static int batch(void *fs,const char *p,int i,vfs_dirent_t *out,int count){
    (void)fs;(void)p;(void)i;for(int j=0;j<count && j<batch_result;j++){memset(out+j,0,sizeof(*out));strcpy(out[j].name,"batch");}
    if(batch_epoch)vfs_namespace_changed();
    return batch_result;
}
static char filename[128],destination[256];
void *kmalloc(size_t n){allocations++;return malloc(n);}
void kfree(void *p){free(p);}
void ksnprintf(char *b,size_t n,const char *f,...){va_list a;va_start(a,f);vsnprintf(b,n,f,a);va_end(a);}
int blk_is_usb(int d){return d>=4;}
void fat_reset_mounts(void){}
void ntfs_reset_mounts(void){}
static int list(void *fs,const char *path,int i,vfs_dirent_t *out){
    (void)fs;(void)path;list_calls++;
    if(list_epoch)vfs_namespace_changed();
    if(i==list_failure)return -1;
    if(i>(filename[0]?1:0))return 0;
    memset(out,0,sizeof(*out));strcpy(out->name,!filename[0] || i?"taken.txt":filename);
    if(bad_name==1)memset(out->name,'x',sizeof(out->name));else if(bad_name==2)out->name[0]=0;
    return list_return;
}
static void space(void *fs,u32 *total,u32 *free_kb){(void)fs;*total=16;*free_kb=8;if(space_epoch)vfs_namespace_changed();}
static int read_file(void *fs,const char *p,u32 off,void *b,u32 n){
    (void)fs;(void)p;(void)off;reads++;if(read_result>0 && n)((char *)b)[0]='x';if(read_epoch)vfs_namespace_changed();return read_result;
}
static int remove_file(void *fs,const char *p){
    (void)fs;(void)p;removes++;if(!result || published)filename[0]=0;if(remove_epoch)vfs_namespace_changed();return result;
}
static int write_file(void *fs,const char *p,const void *b,u32 n){(void)fs;(void)p;(void)b;return (int)n;}
static int rename_file(void *fs,const char *from,const char *to){
    (void)fs;(void)from;renames++;strcpy(destination,to);
    if(!result || published){const char *base=to;for(const char *p=to;*p;p++)if(*p=='/')base=p+1;strcpy(filename,base);}
    if(epoch)vfs_init();
    return result;
}
static int mkdir_file(void *fs,const char *p){(void)fs;mkdirs++;strcpy(destination,p);return result;}
static const vfs_ops_t ops={.readdir=list,.write=write_file,.mkdir=mkdir_file,.rename=rename_file,.read=read_file,.remove=remove_file,.space=space};
static void fresh(void){
    renames=mkdirs=result=published=epoch=allocations=0;
    reads=removes=list_calls=list_epoch=bad_name=space_epoch=read_epoch=remove_epoch=0;list_failure=-1;list_return=read_result=1;strcpy(filename,"file.txt");destination[0]=0;
    fm_input_mode=fm_confirm=fm_view_open=fm_inited=0;fm_active=0;
    vfs_init();assert(vfs_mount("source",FS_RAMFS,&ops,NULL,-1,0,0,"Source",1)>=0);fm_init();
}
static void type(const char *name){fm_key(21,10);for(const char *p=name;*p;p++)fm_key(*p,10);}
int main(void){
    for(int lang=0;lang<2;lang++){
        kv_current_lang=lang?KV_LANG_RU:KV_LANG_EN;
        fresh();fm_ask_rename();assert(fm_input_active() && fm_input_selected() && !strcmp(fm_input_text(),"file.txt"));
        fm_key(KEY_UP,10);assert(fm_input_selected());fm_key('n',10);assert(!strcmp(fm_input_text(),"n"));
        type("new.txt");fm_input_submit();assert(renames==1 && !mkdirs && !allocations && !fm_input_active() && !strcmp(destination,"/new.txt") && !strcmp(act()->items[act()->sel].name,"new.txt"));
        fresh();fm_ask_rename();fm_input_submit();assert(renames==1 && !strcmp(destination,"/file.txt"));
        fresh();fm_ask_rename();type("new.txt");fm_input_cancel();assert(!renames && !fm_input_active());
        fresh();fm_ask_rename();fm_key(27,10);assert(!renames && !fm_input_active());
        const char *invalid[]={"",".","..","a/b","a\\b"};
        for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
            fresh();fm_ask_rename();type(invalid[i]);fm_input_submit();assert(!renames && fm_input_active());
        }
        fresh();fm_ask_rename();type("taken.txt");result=-5;fm_input_submit();assert(renames==1 && fm_input_active() && !strcmp(filename,"file.txt"));
        result=0;type("new.txt");fm_input_submit();assert(renames==2 && !fm_input_active());
        fresh();fm_ask_rename();vfs_find("source")->writable=0;fm_input_submit();assert(!renames && fm_input_active());
        for(int change=0;change<4;change++){
            fresh();fm_ask_rename();type("new.txt");
            if(!change)fm_refresh();else if(change==1)vfs_init();else if(change==2)fm_set_active(1);else strcpy(act()->path,"/mnt/source/changed");
            fm_input_submit();assert(!renames && !fm_input_active());
        }
        for(int mode=0;mode<2;mode++){
            fresh();fm_ask_rename();type("new.txt");result=-6;published=mode;fm_input_submit();
            assert(renames==1 && !fm_input_active() && fm_status_col==0xFFCC3344 && !strcmp(filename,mode?"new.txt":"file.txt"));
        }
        fresh();fm_ask_rename();type("new.txt");epoch=1;fm_input_submit();assert(renames==1 && !fm_input_active() && fm_status_col==0xFFCC3344);
        fresh();fm_ask_rename();fm_input_codepoint(0xd800);assert(fm_input_selected());
        fm_input_codepoint(0x444);fm_input_codepoint(0x430);assert(!strcmp(fm_input_text(),"фа"));
        fm_key('\b',10);assert(!strcmp(fm_input_text(),"ф"));fm_key('\b',10);assert(!fm_input_text()[0]);
        for(int i=0;i<140;i++)fm_key('x',10);
        assert(strlen(fm_input_text())==127);
        fm_input_codepoint(0x444);assert(strlen(fm_input_text())==127);fm_key(1,10);fm_key('\b',10);assert(!fm_input_text()[0]);
        fresh();panes[0].items[0].is_dir=1;fm_ask_rename();assert(!fm_input_active());
        fresh();panes[0].sel=-1;fm_ask_rename();assert(!fm_input_active());
        fresh();memset(panes[0].path,'/',sizeof(panes[0].path));fm_ask_rename();assert(!fm_input_active());fm_ask_mkdir();assert(!fm_input_active());
        fresh();memset(panes[0].items[0].name,'x',sizeof(panes[0].items[0].name));fm_ask_rename();assert(!fm_input_active());
        fresh();memset(panes[0].path,'x',253);memcpy(panes[0].path,"/mnt/source/",12);panes[0].path[253]=0;strcpy(panes[0].items[0].name,"f");
        fm_ask_rename();assert(fm_input_active());type("longer");fm_input_submit();assert(!renames && fm_input_active());
        fresh();fm_ask_mkdir();type("newdir");fm_input_submit();assert(mkdirs==1 && !renames && !fm_input_active() && !strcmp(destination,"/newdir"));
        fresh();fm_key(KEY_F7,10);assert(fm_input_active());fm_refresh();fm_input_submit();assert(!mkdirs && !fm_input_active());
    }
    for(int language=0;language<2;language++) {
        kv_current_lang=language?KV_LANG_RU:KV_LANG_EN;
        fresh();assert(fm_listing_current(0) && fm_listing_current(1));u32 total,free_kb;fm_pane_space(0,&total,&free_kb);assert(total==16 && free_kb==8);vfs_namespace_changed();
        fm_pane_space(0,&total,&free_kb);assert(!total && !free_kb);
        assert(!fm_listing_current(0) && !fm_pane_count(0) && !fm_pane_item(0,0));
        fm_activate();fm_do_copy();fm_ask_delete();fm_ask_rename();fm_ask_mkdir();
        assert(!reads && !removes && !renames && !allocations && !fm_confirm && !fm_input_mode && !fm_view_open);
        fm_refresh();assert(fm_listing_current(0) && fm_listing_current(1));
        fresh();pane_namespace[1]=0;fm_do_copy();assert(!reads && !allocations);
        fresh();list_failure=1;vfs_dirent_t entries[2];assert(vfs_readdir_many("/mnt/source",0,entries,2)==-1);
        fresh();list_epoch=1;assert(vfs_readdir_many("/mnt/source",0,entries,2)==-1);
        for(int r=-1;r<=3;r++)for(int change=0;change<2;change++) {
            fresh();vfs_ops_t batched=ops;batched.readdir_many=batch;vfs_find("source")->ops=&batched;batch_result=r;batch_epoch=change;
            assert(vfs_readdir_many("/mnt/source",0,entries,2)==(!change && r>=0 && r<=2?r:-1));
        }
        for(int fault=0;fault<6;fault++) {
            fresh();list_calls=0;
            if(!fault)list_epoch=1;else if(fault==1)space_epoch=1;else if(fault==2)list_failure=1;else if(fault==3)bad_name=1;else if(fault==4)list_return=2;else bad_name=2;
            u32 before=listing_epoch;pane_load(&panes[0]);
            assert(!fm_listing_current(0) && !panes[0].count && !panes[0].loaded && !panes[0].total_kb && !panes[0].free_kb && listing_epoch!=before);
            assert(list_calls==(fault==1?0:fault==2?2:1));
            fm_activate();fm_ask_delete();fm_ask_rename();assert(!reads && !removes && !renames && !fm_confirm && !fm_input_mode);
        }
        for(int change=0;change<5;change++) {
            fresh();fm_ask_delete();assert(fm_confirm_pending());
            if(!change)vfs_namespace_changed();else if(change==1)fm_refresh();else if(change==2)fm_set_active(1);else if(change==3)strcpy(act()->path,"/mnt/source/elsewhere");else act()->sel=1;
            fm_confirm_yes();assert(!removes && !fm_confirm_pending());
        }
        fresh();fm_ask_delete();fm_key('y',10);assert(removes==1 && !fm_confirm_pending());
        fresh();fm_ask_delete();fm_confirm_no();assert(!removes);
        for(int mode=0;mode<2;mode++) {
            fresh();fm_ask_delete();result=-2;remove_epoch=1;published=mode;fm_confirm_yes();
            assert(removes==1 && !fm_confirm_pending() && fm_status_col==0xFFCC3344 && !strcmp(filename,mode?"":"file.txt"));
        }
        fresh();read_epoch=1;fm_activate();assert(reads==1 && !fm_view_open && !fm_view_len);
        fresh();read_result=4096;fm_activate();assert(reads==1 && !fm_view_open && !fm_view_len);
        fresh();read_result=-1;fm_activate();assert(reads==1 && !fm_view_open);
        fresh();fm_activate();assert(reads==1 && fm_view_open && fm_view_len==1);fm_close_view();
        fresh();memset(act()->path,'x',250);memcpy(act()->path,"/mnt/source/",12);act()->path[250]=0;
        fm_activate();fm_ask_delete();assert(!reads && !removes && !fm_confirm_pending());
        fresh();act()->count=FM_MAX_ENTRIES+1;act()->sel=FM_MAX_ENTRIES;
        fm_activate();fm_ask_delete();fm_ask_rename();fm_do_copy();assert(!reads && !removes && !renames && !allocations);
    }
    puts("PASS rename/mkdir modal EN/RU: snapshot listing/namespace/pane/path; bounds/UTF8 edit/selection/cancel; conflict/RO; no allocation, unknown publication/epoch, reselect, no copy-delete; namespace-at-load/mid-list errors/stale actions, bounded preview/delete snapshots/uncertain deletion PASS");
}
