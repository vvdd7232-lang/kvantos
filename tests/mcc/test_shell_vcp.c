/* Actual shell/vfs copy; host-only callbacks, allocation and error injection. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/shell.c"
#include "../../kernel/vfs.c"
kv_lang_t kv_current_lang=KV_LANG_EN;
static int reads,writes,stats,sizes,allocs,live,oom,stat_epoch,size_epoch,alloc_epoch,read_epoch,write_epoch,published;
static int read_result,write_result,accept_any,removes,mkdirs,mutation_epoch,chars;
u32 utf8_len(const char *s){u32 n=0;while(*s)if((*(s++)&0xc0)!=0x80)n++;return n;}
void kputc(char c){(void)c;chars++;}
void vga_set_color(u8 c){(void)c;}
static u32 source_size,target_size;
static char target[64],last_path[256],directory_path[256],output[2048];
void *kmalloc(size_t n){allocs++;if(oom)return NULL;void *p=malloc(n);assert(p);live++;if(alloc_epoch)vfs_namespace_changed();return p;}
void kfree(void *p){if(p){live--;free(p);}}
void kputs(const char *s){assert(strlen(output)+strlen(s)<sizeof(output));strcat(output,s);}
void kprintf(const char *s,...){char b[1024];va_list a;va_start(a,s);vsnprintf(b,sizeof(b),s,a);va_end(a);kputs(b);}
void ksnprintf(char *b,size_t n,const char *f,...){va_list a;va_start(a,f);vsnprintf(b,n,f,a);va_end(a);}
int blk_is_usb(int d){return d>=4;}
void fat_reset_mounts(void){}
void ntfs_reset_mounts(void){}
static int stat_file(void *f,const char *p){
    (void)f;stats++;if(stat_epoch==stats)vfs_namespace_changed();
    if(!strcmp(p,"/") || (directory_path[0] && !strcmp(directory_path,p)))return 2;
    return accept_any || !strcmp(p,"/source") || !strcmp(p,"/target");
}
static u32 size_file(void *f,const char *p){(void)f;(void)p;sizes++;if(size_epoch)vfs_namespace_changed();return source_size;}
static int read_file(void *f,const char *p,u32 off,void *b,u32 n){
    (void)f;(void)p;assert(!off);reads++;u32 k=n>7?7:n;if(k)memcpy(b,"payload",k);if(read_epoch)vfs_namespace_changed();return read_result;
}
static int write_file(void *f,const char *p,const void *b,u32 n){
    (void)f;writes++;strcpy(last_path,p);assert(n<=sizeof(target));
    if(write_result>=0 || published){u32 k=write_result>=0 && (u32)write_result<n?(u32)write_result:n;memcpy(target,b,k);target_size=k;}
    if(write_epoch)vfs_namespace_changed();
    return write_result;
}
static int remove_file(void *f,const char *p){(void)f;strcpy(last_path,p);removes++;if(mutation_epoch)vfs_namespace_changed();return 0;}
static int mkdir_file(void *f,const char *p){(void)f;strcpy(last_path,p);mkdirs++;if(mutation_epoch)vfs_namespace_changed();return 0;}
static const vfs_ops_t ops={.stat=stat_file,.size=size_file,.read=read_file,.write=write_file,.remove=remove_file,.mkdir=mkdir_file};
static void fresh(void){
    removes=mkdirs=mutation_epoch=chars=0;
    assert(!live);reads=writes=stats=sizes=allocs=oom=stat_epoch=size_epoch=alloc_epoch=read_epoch=write_epoch=published=accept_any=0;
    read_result=write_result=7;source_size=7;target_size=3;memcpy(target,"OLD",3);last_path[0]=directory_path[0]=output[0]=0;strcpy(sh_cwd,"/mnt/ram");
    vfs_init();assert(vfs_mount("ram",FS_RAMFS,&ops,NULL,-1,0,0,"RAM",1)>=0);assert(vfs_mount("disk",FS_KVFS,&ops,NULL,0,2048,0,"Disk",1)>=0);
}
static void copy(const char *from,const char *to){char *args[]={"vcp",(char *)from,(char *)to};cmd_vcp(3,args);assert(!live);}
static void intact(void){assert(!writes && target_size==3 && !memcmp(target,"OLD",3));}
static int copied(void){return strstr(output,kv_current_lang==KV_LANG_RU?"Скопировано":"Copied")!=NULL;}
int main(void){
    for(int lang=0;lang<2;lang++){
        kv_current_lang=lang?KV_LANG_RU:KV_LANG_EN;
        fresh();copy("source","/mnt/disk/target");assert(reads==1 && writes==1 && allocs==1 && target_size==7 && !memcmp(target,"payload",7) && copied());
        fresh();copy("source","/mnt/disk");assert(writes==1 && !strcmp(last_path,"/source") && copied());
        fresh();sh_cwd[0]=0;copy("ram/source","disk/target");assert(writes==1 && copied());
        fresh();strcpy(sh_cwd,"/mnt/ram/");copy("source","/mnt/disk/target");assert(writes==1 && copied());
        const int bad_reads[]={-1,0,3,8,2147483647};
        for(unsigned i=0;i<sizeof(bad_reads)/sizeof(*bad_reads);i++){fresh();read_result=bad_reads[i];copy("source","/mnt/disk/target");intact();assert(reads==1 && !copied());}
        for(int stage=1;stage<=5;stage++){
            fresh();if(stage<=2)stat_epoch=stage;else if(stage==3)size_epoch=1;else if(stage==4)alloc_epoch=1;else read_epoch=1;
            copy("source","/mnt/disk/target");intact();assert(!copied() && reads==(stage==5) && allocs==(stage>=4));
        }
        fresh();oom=1;copy("source","/mnt/disk/target");intact();assert(!reads && allocs==1);
        fresh();vfs_find("disk")->writable=0;copy("source","/mnt/disk/target");intact();assert(!reads && !allocs);
        fresh();copy("source","/mnt/missing/target");intact();assert(!reads && !allocs);
        fresh();copy("missing","/mnt/disk/target");intact();assert(!reads && !allocs);
        fresh();copy("source","source");intact();assert(!reads && !allocs && !sizes && !copied());
        fresh();vfs_find("ram")->writable=0;oom=1;copy("source","/mnt/ram");intact();assert(!reads && !allocs && !sizes);
        fresh();source_size=4u*1024*1024+1;copy("source","/mnt/disk/target");intact();assert(!reads && !allocs);
        fresh();source_size=0;read_result=write_result=0;copy("source","/mnt/disk/target");assert(reads==1 && writes==1 && !target_size && copied());
        const int bad_writes[]={-1,-2,-3,0,3,8};
        for(unsigned i=0;i<sizeof(bad_writes)/sizeof(*bad_writes);i++){fresh();write_result=bad_writes[i];copy("source","/mnt/disk/target");assert(writes==1 && !copied());}
        for(int mode=0;mode<2;mode++){fresh();write_epoch=1;if(mode){published=1;write_result=-1;}copy("source","/mnt/disk/target");assert(writes==1 && target_size==7 && !memcmp(target,"payload",7) && !copied());}
        char longarg[257];memset(longarg,'x',256);longarg[256]=0;
        fresh();copy(longarg,"/mnt/disk/target");intact();assert(!stats && !allocs);
        fresh();copy("source",longarg);intact();assert(!stats && !allocs);
        fresh();memcpy(longarg,"/mnt/ram/",9);longarg[255]=0;accept_any=1;copy(longarg,"/mnt/disk/target");assert(writes==1 && copied()); /* exact 255-byte path, synthetic backend */
        fresh();memset(sh_cwd,'x',sizeof(sh_cwd));copy("source","/mnt/disk/target");intact();assert(!stats && !allocs);
        fresh();memset(sh_cwd,'x',249);sh_cwd[249]=0;copy("source","/mnt/disk/target");intact();assert(!stats && !allocs);
        for(int len=245;len<=255;len++){
            fresh();char dest[256];memcpy(dest,"/mnt/disk/",10);memset(dest+10,'d',len-10);dest[len]=0;strcpy(directory_path,dest+9);
            copy("source",dest);int ok=len+1+6<256;assert(writes==ok && allocs==ok && copied()==ok);if(!ok)intact();
        }
        fresh();char *args[]={"vcp"};cmd_vcp(1,args);intact();assert(!stats);
        fresh();copy("","/mnt/disk/target");intact();assert(!stats);
        fresh();char path[256];assert(!sh_path(path,NULL));memset(longarg,'x',256);assert(!sh_path(path,longarg));
    }
    for(int lang=0;lang<2;lang++) {
        kv_current_lang=lang?KV_LANG_RU:KV_LANG_EN;
        const int results[]={-1,0,3,7,8,2147483647};
        for(unsigned i=0;i<sizeof(results)/sizeof(*results);i++) {
            fresh();read_result=results[i];char *a[]={"vcat","source"};cmd_vcat(2,a);
            assert(!live && reads==1 && chars==(results[i]==7?7:0));
        }
        for(int stage=0;stage<4;stage++) {
            fresh();if(!stage)stat_epoch=1;else if(stage==1)size_epoch=1;else if(stage==2)alloc_epoch=1;else read_epoch=1;
            char *a[]={"vcat","source"};cmd_vcat(2,a);assert(!live && !chars);
        }
        fresh();source_size=0;read_result=0;char *cat[]={"vcat","source"};cmd_vcat(2,cat);assert(!live && !chars && reads==1);
        fresh();oom=1;cmd_vcat(2,cat);assert(!reads && !live);
        for(int cmd=0;cmd<5;cmd++) {
            fresh();memset(sh_cwd,'x',250);memcpy(sh_cwd,"/mnt/ram/",9);sh_cwd[250]=0;
            char *a[]={"test","victim"};
            if(!cmd)cmd_vrm(2,a);else if(cmd==1)cmd_vmkdir(2,a);else if(cmd==2)cmd_vcat(2,a);else if(cmd==3)cmd_vls(2,a);else cmd_vcd(2,a);
            assert(!removes && !mkdirs && !reads && !stats && !allocs);
        }
        fresh();char *rm[]={"vrm","target"};cmd_vrm(2,rm);assert(removes==1 && !strcmp(last_path,"/target"));
        fresh();mutation_epoch=1;cmd_vrm(2,rm);assert(removes==1 && strstr(output,lang?"неизвестен":"uncertain"));
        fresh();char *mk[]={"vmkdir","folder"};cmd_vmkdir(2,mk);assert(mkdirs==1 && !strcmp(last_path,"/folder"));
        fresh();mutation_epoch=1;cmd_vmkdir(2,mk);assert(mkdirs==1 && strstr(output,lang?"неизвестен":"uncertain"));
        fresh();char *cd[]={"vcd","/mnt/disk"};stat_epoch=1;cmd_vcd(2,cd);assert(!strcmp(sh_cwd,"/mnt/ram"));
        fresh();cmd_vcd(2,cd);assert(!strcmp(sh_cwd,"/mnt/disk"));
        fresh();memset(sh_cwd,'x',sizeof(sh_cwd));char *up[]={"vcd",".."};cmd_vcd(2,up);assert(!stats);
    }
    puts("PASS actual shell vcat/vrm/vmkdir/vcd/vls EN/RU: read bounds/exact preview/no display after epoch, OOM/empty; long CWD no callbacks, mutation uncertainty, stale cd refusal");
    puts("PASS actual shell vcp EN/RU: exact-read negative/short/overcount, namespace at stat/size/alloc/read/write, no blind retry; samepath/directory no-allocation, RO/OOM/missing/empty/limit, bounded absolute/CWD/directory joins, partial/unknown write is not success; no remove path");
}
