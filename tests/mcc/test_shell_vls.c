/* Actual shell consumer must never treat -1 as another row. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/shell.c"
#include "../../kernel/vfs.c"
kv_lang_t kv_current_lang=KV_LANG_EN;
static char output[2048];static int calls,fail_at,change_at,bad;
void kputs(const char *s){assert(strlen(output)+strlen(s)<sizeof(output));strcat(output,s);}
void kprintf(const char *s,...){char b[1024];va_list a;va_start(a,s);vsnprintf(b,sizeof(b),s,a);va_end(a);kputs(b);}
void ksnprintf(char *b,size_t n,const char *f,...){va_list a;va_start(a,f);vsnprintf(b,n,f,a);va_end(a);}
void vga_set_color(u8 c){(void)c;}
u32 utf8_len(const char *s){u32 n=0;while(*s)if((*(s++)&0xc0)!=0x80)n++;return n;}
int blk_is_usb(int d){return d>=4;}
void fat_reset_mounts(void){}
void ntfs_reset_mounts(void){}
static int list(void *fs,const char *path,int i,vfs_dirent_t *out){
    (void)fs;(void)path;calls++;if(calls==change_at)vfs_namespace_changed();
    if(calls==fail_at)return -1;
    if(i==1)return 0;
    memset(out,0,sizeof(*out));strcpy(out->name,"entry");out->size=7;
    if(bad==1)memset(out->name,'x',sizeof(out->name));else if(bad==2)out->name[0]=0;
    return bad==3?2:1;
}
static const vfs_ops_t ops={.readdir=list};
static void fresh(void){output[0]=0;calls=fail_at=change_at=bad=0;vfs_init();assert(vfs_mount("test",FS_RAMFS,&ops,NULL,-1,0,0,"Test",1)>=0);}
static void show(const char *path){char *a[]={"vls",(char *)path};cmd_vls(2,a);}
int main(void){
    for(int lang=0;lang<2;lang++){
        kv_current_lang=lang?KV_LANG_RU:KV_LANG_EN;
        fresh();show("/mnt/test");assert(calls==2 && strstr(output,lang?"объектов: 1":"1 entries"));
        for(int fault=0;fault<7;fault++){
            fresh();if(fault<2)fail_at=fault+1;else if(fault<4)change_at=fault-1;else bad=fault-3;
            show("/mnt/test");assert(calls<=2 && strstr(output,lang?"вывод неполный":"output incomplete") && !strstr(output,lang?"объектов:":"entries,"));
        }
        fresh();show("/mnt/missing");assert(!calls && strstr(output,lang?"вывод неполный":"output incomplete"));
    }
    puts("PASS actual shell vls EN/RU: strict EOF/error before/after a row, namespace/malformed result/name, unknown mount; bounded termination, partial-output warning without misleading count");
}
