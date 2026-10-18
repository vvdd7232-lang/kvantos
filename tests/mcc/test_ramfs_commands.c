/* Actual shell cp/mv + RAMFS: regressions for destructive pre-delete. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/ramfs.c"
#include "../../kernel/utilcmds.c"
kv_lang_t kv_current_lang=KV_LANG_EN;
static int oom,live,allocs,frees,rename_calls;
void *kmalloc(size_t n){allocs++;if(oom)return NULL;void *p=malloc(n);assert(p);live++;return p;}
void kfree(void *p){if(p){live--;frees++;free(p);}}
void kputs(const char *s){(void)s;}
void kprintf(const char *s,...){(void)s;}
int vfs_rename(const char *from,const char *to){(void)from;(void)to;rename_calls++;return -7;}
static void fresh(void){
    for(int i=0;i<RAMFS_MAX_FILES;i++)if(files[i].used){char name[24];strcpy(name,files[i].name);assert(!ramfs_delete(name));}
    assert(!live);oom=allocs=frees=rename_calls=0;
    assert(!ramfs_write("source","payload",7) && !ramfs_write("target","KEEP",4));
}
static void copy(const char *from,const char *to){char *args[]={"cp",(char *)from,(char *)to};cmd_cp(3,args);}
static void move(const char *from,const char *to){char *args[]={"mv",(char *)from,(char *)to};cmd_mv(3,args);}
static void body(const char *name,const char *bytes,u32 n){rfile_t *f=ramfs_find(name);assert(f && f->size==n && !memcmp(f->data,bytes,n));}
int main(void){
    for(int language=0;language<2;language++){
        kv_current_lang=language?KV_LANG_RU:KV_LANG_EN;
        fresh();char *target=ramfs_find("target")->data;oom=1;copy("source","target");
        body("source","payload",7);body("target","KEEP",4);assert(ramfs_find("target")->data==target && live==2 && !frees);
        int before=allocs;copy("source","source");body("source","payload",7);assert(allocs==before && live==2);
        oom=0;copy("source","target");body("source","payload",7);body("target","payload",7);assert(live==2);
        assert(!ramfs_write("empty","",0));copy("empty","target");body("target","",0);body("source","payload",7);
        copy("missing","target");body("target","",0);copy("source","xxxxxxxxxxxxxxxxxxxxxxxx");assert(!ramfs_find("xxxxxxxxxxxxxxxxxxxxxxxx"));
        for(int i=0;i<RAMFS_MAX_FILES-3;i++){char name[24];snprintf(name,sizeof(name),"f%d",i);assert(!ramfs_write(name,"x",1));}
        copy("source","target");body("target","payload",7);assert(live==RAMFS_MAX_FILES);
        before=allocs;copy("source","new");assert(!ramfs_find("new") && allocs==before && live==RAMFS_MAX_FILES);
        oom=1;move("source","renamed");body("renamed","payload",7);assert(!ramfs_find("source") && live==RAMFS_MAX_FILES);
        target=ramfs_find("target")->data;move("renamed","target");body("target","payload",7);body("renamed","payload",7);assert(ramfs_find("target")->data==target);
        move("/ram/renamed","/kv/target");assert(rename_calls==1 && live==RAMFS_MAX_FILES);
    }
    fresh();assert(!ramfs_delete("source") && !ramfs_delete("target") && !live);
    puts("PASS actual shell cp/mv EN/RU: OOM target retained, samepath no allocation, COW overwrite/empty/missing/long/full table, allocation-free no-replace mv, no crossvolume copy-delete");
}
