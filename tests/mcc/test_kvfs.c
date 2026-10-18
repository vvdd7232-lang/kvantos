#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/kvfs.c"
#define NSECT 4096
static unsigned char image[NSECT*512],base[NSECT*512],snapshot[NSECT*512];
static int calls,fail_at,after_write,reads,read_failure;
static u32 namespace_changes;
void vfs_namespace_changed(void){namespace_changes++;}
int ata_boot_drive(void) { return 0; }
u32 ata_sectors(int i) { return i==0?NSECT:0; }
int ata_read(int d,u32 l,u8 n,void *b) {
    assert(d==0 && l<NSECT && n<=NSECT-l);reads++;if(read_failure)return -1;memcpy(b,image+l*512,n*512);return 0;
}
int ata_write(int d,u32 l,u8 n,const void *b) {
    assert(d==0 && l<NSECT && n<=NSECT-l); calls++;
    if(calls==fail_at && !after_write) return -1;
    memcpy(image+l*512,b,n*512);
    return calls==fail_at?-1:0;
}
static void fresh(void) {
    fail_at=calls=reads=read_failure=0;after_write=0;memset(image,0,sizeof(image));assert(!kvfs_format());
    assert(!kvfs_write("old.txt","old",3,0)); memcpy(base,image,sizeof(image));calls=0;
}
static void old(void) { char b[8]={0};assert(kvfs_exists("old.txt") && kvfs_size("old.txt")==3);assert(kvfs_read("old.txt",b,8)==3 && !memcmp(b,"old",3)); }
int main(void) {
    fresh();old();
    unsigned char body[1537];for(unsigned i=0;i<sizeof(body);i++) body[i]=(unsigned char)(i*7);
    /* Fail each data sector and the one metadata sector, both before and after media modification. */
    for(int mode=0;mode<2;mode++) for(int fault=1;fault<=5;fault++) {
        memcpy(image,base,sizeof(image));fail_at=0;assert(!kvfs_mount());
        u32 prior_generation=namespace_changes;
        calls=0;fail_at=fault;after_write=mode;
        assert(kvfs_write("old.txt",body,sizeof(body),0)==-2);
        assert(calls==fault && namespace_changes==prior_generation+(fault==5)); /* fence invalidates, no retry */
        if(fault<5) { assert(kvfs_mounted());old(); }
        else { assert(!kvfs_mounted());assert(kvfs_write("later.txt","x",1,0)==-1); }
        fail_at=0;assert(!kvfs_mount());
        if(mode && fault==5) { unsigned char got[1537];assert(kvfs_read("old.txt",got,sizeof(got))==(int)sizeof(got));assert(!memcmp(got,body,sizeof(got))); }
        else old();
    }
    fresh();
    /* Fully occupied medium: growing or shrinking cannot reclaim the live old extent. */
    for(int i=0;i<KVFS_MAX_FILES;i++) dir[i].flags=0;
    dir[0]=(kvfs_dirent_t){.name="old.txt",.start=DATA_START,.sectors=NSECT-DATA_START,.size=3,.flags=1};
    memcpy(image+DATA_START*512,"old",3);assert(!write_dir());
    memcpy(snapshot,image,sizeof(image));calls=0;
    assert(kvfs_write("old.txt",body,sizeof(body),0)==-7);old();
    assert(kvfs_write("old.txt","x",1,0)==-7);old();
    assert(!calls && !memcmp(snapshot,image,sizeof(image)));
    assert(kvfs_write("bad",NULL,1,0)==-8 && kvfs_write("bad",body,~0u,0)==-8);
    fresh();assert(!kvfs_write("old.txt","",0,0));assert(!kvfs_size("old.txt"));
    assert(!kvfs_mount() && kvfs_exists("old.txt"));assert(!kvfs_delete("old.txt"));assert(!kvfs_mount() && !kvfs_exists("old.txt"));
    /* Metadata write failure cannot be resurrected by a later operation. */
    fresh();fail_at=1;calls=0;assert(kvfs_delete("old.txt")==-2 && !kvfs_mounted());fail_at=0;assert(!kvfs_mount());old();
    fresh();super.data_start=0;assert(!write_super());assert(kvfs_mount()==-4);
    fresh();dir[0].start=~0u;assert(!write_dir());assert(kvfs_mount()==-4);
    fresh();dir[1]=dir[0];assert(!write_dir());assert(kvfs_mount()==-4);
    fresh();memset(dir[0].name,'x',40);assert(!write_dir());assert(kvfs_mount()==-4);
    fresh();assert(!kvfs_delete("old.txt"));
    /* Rename publishes one metadata sector, never data/superblock/other groups. */
    fresh();dir[0].flags=3;dir[0].reserved[0]=0x12345678;assert(!write_dir());kvfs_dirent_t renamed=dir[0];
    memcpy(snapshot,image,sizeof(image));calls=0;
    assert(!kvfs_rename("old.txt","renamed.txt") && calls==1 && !kvfs_exists("old.txt"));
    memset(renamed.name,0,sizeof(renamed.name));strcpy(renamed.name,"renamed.txt");
    assert(!memcmp(&dir[0],&renamed,sizeof(renamed)) && !memcmp(image,snapshot,DIR_SECTOR*512) && !memcmp(image+(DIR_SECTOR+1)*512,snapshot+(DIR_SECTOR+1)*512,sizeof(image)-(DIR_SECTOR+1)*512));
    assert(!kvfs_mount() && kvfs_exists("renamed.txt") && dir[0].flags==3);
    assert(!kvfs_rename(dir[0].name,dir[0].name+8) && kvfs_exists("txt"));
    char max_name[40];memset(max_name,'x',39);max_name[39]=0;
    assert(!kvfs_rename("txt",max_name) && kvfs_exists(max_name));
    memset(max_name,'x',sizeof(max_name));calls=0;assert(kvfs_rename(max_name,"valid")==-4 && !calls);
    fresh();assert(!kvfs_write("keep.txt","safe",4,0));calls=0;memcpy(snapshot,image,sizeof(image));
    assert(!kvfs_rename("old.txt","old.txt") && !calls);
    assert(kvfs_rename("old.txt","keep.txt")==-5 && !calls);
    const char *invalid[]={NULL,"",".","..","a/b","a\\b","bad\177","xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)assert(kvfs_rename("old.txt",invalid[i])==-4 && !calls);
    assert(kvfs_rename("missing","new.txt")==-3 && !calls && !memcmp(snapshot,image,sizeof(image)));
    /* Failure may be reported before OR after the metadata actually persisted.
       No retry/rollback; mount is fenced until an explicit validated remount. */
    for(int published=0;published<2;published++) {
        fresh();assert(!kvfs_write("keep.txt","safe",4,0));calls=0;fail_at=1;after_write=published;
        assert(kvfs_rename("old.txt","new.txt")==-6 && calls==1 && !kvfs_mounted());
        assert(kvfs_rename("old.txt","new.txt")==-1 && calls==1);
        fail_at=0;assert(!kvfs_mount());assert(kvfs_exists(published?"new.txt":"old.txt") && !kvfs_exists(published?"old.txt":"new.txt"));
        char content[8];assert(kvfs_read(published?"new.txt":"old.txt",content,sizeof(content))==3 && !memcmp(content,"old",3));
        assert(kvfs_read("keep.txt",content,sizeof(content))==4 && !memcmp(content,"safe",4));
    }
    fresh();assert(!kvfs_delete("old.txt"));
    for(int i=0;i<KVFS_MAX_FILES;i++){char name[24];snprintf(name,sizeof(name),"full%d",i);assert(!kvfs_write(name,"",0,0));}
    calls=0;assert(!kvfs_rename("full8","файл.c") && calls==1 && kvfs_file_count()==KVFS_MAX_FILES && kvfs_exists("файл.c"));
    assert(!kvfs_mount() && kvfs_exists("файл.c"));
    fresh();dir[0].sectors=NSECT-super.data_start;assert(!write_dir());calls=0;
    assert(!kvfs_rename("old.txt","allspace.txt") && calls==1); /* no spare extents needed */
    fresh();assert(!kvfs_delete("old.txt"));
    static struct { int valid; unsigned n; unsigned char bytes[2049]; } ref[80];
    unsigned rng=0x512ea31u;
    for(int t=0;t<1500;t++) {
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
        unsigned id=rng%80,n=(rng>>8)%2049;char name[24];snprintf(name,sizeof(name),"f%02u",id);
        if((rng&3)==0) {
            int rc=kvfs_delete(name);assert(rc==(ref[id].valid?0:-3));ref[id].valid=0;
        } else {
            unsigned char data[2049];for(unsigned i=0;i<n;i++) data[i]=(unsigned char)(rng+i*7);
            unsigned count=0;for(unsigned i=0;i<80;i++) count+=(unsigned)ref[i].valid;
            int rc=kvfs_write(name,data,n,0);
            if(!ref[id].valid && count==64) assert(rc==-6);
            else { assert(!rc);ref[id].valid=1;ref[id].n=n;memcpy(ref[id].bytes,data,n); }
        }
        if(t%13==0) assert(!kvfs_mount());
        for(unsigned i=0;i<80;i++) {
            snprintf(name,sizeof(name),"f%02u",i);assert(kvfs_exists(name)==ref[i].valid);
            if(ref[i].valid) { unsigned char data[2049];assert(kvfs_read(name,data,sizeof(data))==(int)ref[i].n);assert(!memcmp(data,ref[i].bytes,ref[i].n)); }
        }
    }
    fresh();memcpy(snapshot,image,sizeof(image));calls=reads=0;u32 prior=namespace_changes;
    assert(!kvfs_mount() && reads==9 && !calls && namespace_changes==prior+1 && !memcmp(snapshot,image,sizeof(image)));
    prior=namespace_changes;read_failure=1;reads=0;
    assert(kvfs_mount()==-2 && !kvfs_mounted() && reads==1 && !calls && namespace_changes==prior+1);
    read_failure=0;image[KVFS_BASE*512]=0;memcpy(snapshot,image,sizeof(image));prior=namespace_changes;
    assert(kvfs_mount()==-3 && !calls && namespace_changes==prior+1 && !memcmp(snapshot,image,sizeof(image)));
    puts("KvFS: COW, full/growing/shrinking/empty, all write-stage faults before/after commit, mount fencing, corrupt extents + no-replace rename/single metadata/unknown-outcome fence/remount/full table+disk/alias/UTF8 +1500 reference-model operations PASS");
}
