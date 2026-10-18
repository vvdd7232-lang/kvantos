/* Native VFS adapter contracts; real RAMFS, mocked KvFS I/O return values. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/ramfs.c"
#include "../../kernel/vfs_native.c"
#include "../../kernel/vfs.c"
static int oom,short_read,rename_calls;static int ready=1;
void *kmalloc(size_t size) { return oom?NULL:malloc(size); }
void kfree(void *ptr) { free(ptr); }
int blk_is_usb(int disk) { return disk>=4; }
void fat_reset_mounts(void) {}
void ntfs_reset_mounts(void) {}
int ata_boot_drive(void) { return 0; }
void heap_stats(u32 *total,u32 *used,u32 *blocks) { *total=1024*1024;*used=*blocks=0; }
int kvfs_mounted(void) { return ready; }
int kvfs_exists(const char *n) { return !strcmp(n,"empty.txt") || !strcmp(n,"old.txt"); }
u32 kvfs_size(const char *n) { return !strcmp(n,"old.txt")?3:0; }
int kvfs_read(const char *n,void *buf,u32 cap) {
    if(!kvfs_exists(n)) return -1;
    u32 count=kvfs_size(n);if(count>cap) count=cap;
    memcpy(buf,"old",count);return short_read?0:(int)count;
}
int kvfs_write(const char *n,const void *data,u32 size,int executable) { (void)n;(void)data;(void)size;(void)executable;return 0; }
int kvfs_rename(const char *from,const char *to) {(void)from;(void)to;rename_calls++;return -6;}
int kvfs_delete(const char *n) { return kvfs_exists(n)?0:-1; }
int kvfs_list(int index,char *name,u32 *size,int *executable) {
    if(!ready || index<0 || index>1)return -1;
    strcpy(name,index?"old.txt":"empty.txt");*size=index?3:0;*executable=0;return 0;
}
void kvfs_stats(u32 *total,u32 *used,u32 *count) { *total=16;*used=1;*count=2; }
int main(void) {
    vfs_init();u32 epoch=vfs_generation();ramfs_vfs_register();kvfs_vfs_register();
    assert(vfs_volume_count()==2);
    assert(vfs_write("/mnt/ram/keep.txt","old",3)==3);
    oom=1;assert(vfs_write("/mnt/ram/keep.txt","new body",8)<0);oom=0;
    char buf[8]={0};assert(vfs_read("/mnt/ram/keep.txt",0,buf,8)==3 && !memcmp(buf,"old",3));
    assert(vfs_write("/mnt/ram/keep.txt","new",3)==3);
    assert(vfs_write("/mnt/ram/keep.txt","nonempty buffer",0)==0 && vfs_size("/mnt/ram/keep.txt")==0);
    assert(vfs_remove("/mnt/ram/keep.txt")==0 && vfs_stat("/mnt/ram/keep.txt")==0);
    assert(vfs_remove("/mnt/ram/keep.txt")<0);
    vfs_dirent_t e;assert(vfs_readdir("/mnt/kv",0,&e)==1 && !strcmp(e.name,"empty.txt"));
    assert(vfs_readdir("/mnt/kv",1,&e)==1 && e.size==3 && !strcmp(e.name,"old.txt"));
    assert(vfs_readdir("/mnt/kv",2,&e)==0);
    ready=0;assert(vfs_readdir("/mnt/kv",0,&e)==-1 && vfs_readdir_many("/mnt/kv",0,&e,1)==-1);ready=1;
    assert(vfs_readdir("/mnt/missing",0,&e)==-1 && vfs_readdir("/mnt/kv",-1,&e)==-1 && vfs_readdir("/mnt/kv",0,NULL)==-1);
    assert(vfs_readdir("/mnt/kv/old.txt",0,&e)==-1 && vfs_readdir("/mnt/ram/file",0,&e)==-1);
    vfs_dirent_t batch[3];
    assert(vfs_readdir_many("/mnt/kv",0,batch,3)==2);
    assert(!strcmp(batch[0].name,"empty.txt") && !strcmp(batch[1].name,"old.txt") && batch[1].size==3);
    assert(vfs_readdir_many("/mnt/kv",1,batch,1)==1 && !strcmp(batch[0].name,"old.txt"));
    assert(vfs_readdir_many("/mnt/kv",2,batch,3)==0);
    assert(vfs_readdir_many("/mnt/missing",0,batch,3)==-1);
    assert(vfs_readdir_many("/mnt/kv",-1,batch,3)==-1);
    assert(vfs_readdir_many("/mnt/kv",0,batch,129)==-1);
    assert(vfs_readdir_many("/mnt/kv",0x7fffffff,batch,1)==-1);
    assert(vfs_readdir_many("/mnt/kv",0,NULL,1)==-1);
    assert(vfs_read("/mnt/kv/empty.txt",0,buf,8)==0);
    assert(vfs_read("/mnt/kv/missing.txt",0,buf,8)<0);
    assert(vfs_read("/mnt/kv/old.txt",1,buf,8)==2 && !memcmp(buf,"ld",2));
    short_read=1;assert(vfs_read("/mnt/kv/old.txt",0,buf,8)<0);short_read=0;
    assert(vfs_write("/mnt/kv/old.txt","new",3)==3 && vfs_remove("/mnt/kv/old.txt")==0);
    vfs_find("kv")->writable=0;assert(vfs_write("/mnt/kv/old.txt","new",3)==-2);
    assert(vfs_remove("/mnt/kv/old.txt")==-2);
    char overlong[VFS_MAX_PATH];memset(overlong,'/',sizeof(overlong));
    assert(vfs_rename(NULL,"/ram/new")==-4 && vfs_rename("/ram/old",NULL)==-4);
    assert(vfs_rename(overlong,"/ram/new")==-4 && vfs_rename("/ram/old",overlong)==-4);
    assert(vfs_write("/mnt/ram/first.txt","payload",7)==7);
    assert(!vfs_rename("/mnt/ram/first.txt","/ram/renamed.txt") && !vfs_stat("/mnt/ram/first.txt") && vfs_size("/ram/renamed.txt")==7);
    assert(vfs_rename("/ram/renamed.txt","/mnt/kv/no.txt")==-7 && !rename_calls);
    assert(vfs_rename("/ram/renamed.txt","/missing/no.txt")==-1 && !rename_calls);
    assert(vfs_rename("/mnt/kv/old.txt","/mnt/kv/new.txt")==-2 && !rename_calls);
    vfs_find("kv")->writable=1;assert(vfs_rename("/mnt/kv/old.txt","/mnt/kv/new.txt")==-6 && rename_calls==1);
    static const vfs_ops_t no_rename={0};
    assert(vfs_mount("unsupported",FS_FAT32,&no_rename,NULL,-1,0,0,"No rename",1)>=0);
    assert(vfs_rename("/unsupported/old","/unsupported/new")==-2 && rename_calls==1);
    assert(vfs_can_rename("/ram/renamed.txt") && !vfs_can_rename("/unsupported/old"));
    assert(vfs_mount("abcdefghijklmno",FS_RAMFS,&no_rename,NULL,-1,0,0,"Long volume",1)>=0);
    assert(vfs_rename("/abcdefghijklmnop","/abcdefghijklmno/new")==-1);
    assert(vfs_stat("/abcdefghijklmnop")==0 && !vfs_can_rename("/abcdefghijklmnop"));
    int mounted_count=vfs_volume_count();generation=0xffffffffu;
    vfs_namespace_changed();assert(vfs_generation()==1 && vfs_volume_count()==mounted_count && vfs_find("ram"));
    vfs_init();assert(vfs_generation()!=epoch && vfs_volume_count()==0);
    puts("VFS native: COW/OOM, byte-count/empty/remove/list/bounded-batch/read-error/RO contracts + namespace generation +no-replace rename/path bounds/RO/unsupported/crossvolume/uncertain propagation PASS");
}
