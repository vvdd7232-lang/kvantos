/* Real file-manager/VFS routing, counted backend faults; no media access. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "../../kernel/filemgr.c"
#include "../../kernel/vfs.c"
kv_lang_t kv_current_lang=KV_LANG_EN;
static struct {unsigned char data[64];u32 size;} source_file,target_file;
static int oom,reads,writes,removes,live_allocs,alloc_calls,read_result,write_result,read_epoch,write_epoch;
void *kmalloc(size_t n) {alloc_calls++;if(oom)return NULL;void *p=malloc(n);assert(p);live_allocs++;return p;}
void kfree(void *p) {if(p){assert(live_allocs>0);live_allocs--;free(p);}}
void ksnprintf(char *buf,size_t size,const char *fmt,...) {va_list ap;va_start(ap,fmt);vsnprintf(buf,size,fmt,ap);va_end(ap);}
int blk_is_usb(int disk) {return disk>=4;}
void fat_reset_mounts(void) {}
void ntfs_reset_mounts(void) {}
static int list(void *fs,const char *p,int i,vfs_dirent_t *out) {(void)fs;(void)p;(void)i;(void)out;return 0;}
static u32 size(void *fs,const char *p) {(void)fs;(void)p;return source_file.size;}
static int read_file(void *fs,const char *p,u32 off,void *buf,u32 max) {
    (void)fs;(void)p;assert(!off);reads++;
    int result=read_result==INT_MIN?(int)source_file.size:read_result;
    u32 n=result<0?0:(u32)result;if(n>max)n=max;if(n>sizeof(source_file.data))n=sizeof(source_file.data);
    memcpy(buf,source_file.data,n);if(read_epoch)vfs_init();
    return result;
}
static int write_file(void *fs,const char *p,const void *buf,u32 n) {
    (void)fs;(void)p;writes++;assert(n<=sizeof(target_file.data));
    int result=write_result==INT_MIN?(int)n:write_result;
    if(result>=0){u32 amount=(u32)result;if(amount>n)amount=n;memcpy(target_file.data,buf,amount);target_file.size=amount;}
    if(write_epoch)vfs_init();
    return result;
}
static int remove_file(void *fs,const char *p) {(void)fs;(void)p;removes++;return 0;}
static const vfs_ops_t ops={.readdir=list,.read=read_file,.size=size,.write=write_file,.remove=remove_file};
static void fresh(void) {
    assert(!live_allocs);oom=reads=writes=removes=alloc_calls=read_epoch=write_epoch=0;
    read_result=write_result=INT_MIN;memset(&source_file,0,sizeof(source_file));memset(&target_file,0,sizeof(target_file));
    memcpy(source_file.data,"payload",7);source_file.size=7;memcpy(target_file.data,"OLD",3);target_file.size=3;
    vfs_init();assert(vfs_mount("source",FS_RAMFS,&ops,NULL,-1,0,0,"Source",1)>=0);
    assert(vfs_mount("target",FS_RAMFS,&ops,NULL,-1,0,0,"Target",1)>=0);
    memset(panes,0,sizeof(panes));fm_active=0;strcpy(panes[0].path,"/mnt/source");strcpy(panes[1].path,"/mnt/target");
    panes[0].loaded=panes[1].loaded=1;pane_namespace[0]=pane_namespace[1]=vfs_generation();
    panes[0].count=1;strcpy(panes[0].items[0].name,"file.txt");fm_status[0]=0;
}
static void copy(int expected_writes) {
    fm_do_copy();assert(writes==expected_writes && !live_allocs && !removes);
    assert(source_file.size==7 && !memcmp(source_file.data,"payload",7));
    if(!expected_writes)assert(target_file.size==3 && !memcmp(target_file.data,"OLD",3));
}
int main(void) {
    for(unsigned language=0;language<2;language++) {
        kv_current_lang=language?KV_LANG_RU:KV_LANG_EN;
        fresh();copy(1);assert(target_file.size==7 && !memcmp(target_file.data,"payload",7));
        for(int got=-3;got<=10;got++)if(got!=7) {fresh();read_result=got;copy(0);assert(reads==1);}
        fresh();read_epoch=1;copy(0);assert(reads==1);
        fresh();write_epoch=1;copy(1);assert(fm_status_col==0xFFCC3344);
        fresh();write_result=3;copy(1);assert(fm_status_col==0xFFCC3344);
        fresh();write_result=-2;copy(1);assert(fm_status_col==0xFFCC3344);
        fresh();oom=1;copy(0);assert(!reads);
        fresh();vfs_find("target")->writable=0;copy(0);assert(!reads && !alloc_calls);
        fresh();strcpy(panes[1].path,panes[0].path);copy(0);assert(!reads && !alloc_calls);
        fresh();panes[0].items[0].is_dir=1;copy(0);assert(!reads && !alloc_calls);
        fresh();source_file.size=FM_COPY_LIMIT+1;fm_do_copy();assert(!reads && !writes && !alloc_calls);
        const char *bad[]={"",".","..","a/b","a\\b"};
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){fresh();strcpy(panes[0].items[0].name,bad[i]);copy(0);assert(!reads && !alloc_calls);}
        fresh();memset(panes[0].path,'x',sizeof(panes[0].path));copy(0);assert(!reads && !alloc_calls);
        fresh();memset(panes[0].items[0].name,'x',sizeof(panes[0].items[0].name));copy(0);assert(!reads && !alloc_calls);
        /* Exact path255 accepted,256 refused before read/write/allocation. */
        fresh();unsigned prefix=VFS_MAX_PATH-1-1-(unsigned)strlen("file.txt");
        memset(panes[0].path,'x',prefix);memcpy(panes[0].path,"/mnt/source/",12);panes[0].path[prefix]=0;copy(1);
        fresh();memset(panes[1].path,'x',prefix+1);memcpy(panes[1].path,"/mnt/target/",12);panes[1].path[prefix+1]=0;copy(0);assert(!reads && !alloc_calls);
        fresh();source_file.size=0;fm_do_copy();assert(writes==1 && !target_file.size && !live_allocs);
        fresh();panes[0].sel=1;copy(0);assert(!alloc_calls);
    }
    fresh();listing_epoch=0xffffffffu;pane_load(&panes[1]);assert(fm_listing_epoch()==1);
    u32 revision=fm_listing_epoch();copy(1);assert(fm_listing_epoch()!=revision);
    revision=fm_listing_epoch();fm_refresh();assert(fm_listing_epoch()!=revision && fm_listing_epoch());
    unsigned seed=715;
    for(unsigned i=0;i<2000;i++) {
        fresh();seed=seed*1664525u+1013904223u;read_result=(int)(seed%18)-4;oom=(seed&31)==0;
        copy(!oom && read_result==7);assert(!live_allocs);
    }
    puts("PASS file-manager copy exact reads/zero/short/error/oversize, RO/OOM, path/name bounds, same path, epoch changes, uncertain writes, no source delete;2000 mutations,EN/RU");
    return 0;
}
