/* Actual VFS, procfs, scheduler-copy and cat paths with deterministic devices.
   Snapshot IRQ guards are disabled ONLY in this host binary. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define KV_SCHED_HOSTTEST
#define KV_PROCFS_HOSTTEST
#include "../../kernel/sched.c"
#include "../../kernel/procfs.c"
#include "../../kernel/vfs.c"
#include "../../kernel/shell.c"

kv_lang_t kv_current_lang=KV_LANG_EN;
static task_t ring[257];
static u32 physical_total=65536,physical_used=1024,hz=100;
static u64 clock_ticks=123456789;
static char console[16384];static size_t console_length;
static int disk_ops;
u32 pmm_total_frames(void) { return physical_total; }
u32 pmm_used_frames(void) { return physical_used; }
u64 timer_ticks(void) { return clock_ticks; }
u32 timer_hz(void) { return hz; }
void cpu_vendor(char *out) { strcpy(out,"GenuineIntel"); }
void cpu_brand(char *out) { strcpy(out,"Test processor"); }
int blk_is_usb(int disk) { (void)disk;disk_ops++;return 0; }
void fat_reset_mounts(void) {}
void ntfs_reset_mounts(void) {}
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
rfile_t *ramfs_find(const char *name) {
    static rfile_t file={.name="hello.txt",.data="legacy RAM\n",.size=11,.used=1};
    return !strcmp(name,file.name)?&file:NULL;
}
void vga_set_color(u8 c) { (void)c; }
void vga_putc(char c) { assert(console_length+1<sizeof(console));console[console_length++]=c;console[console_length]=0; }
void serial_putc(char c) { (void)c; }

static void setup_ring(unsigned count) {
    memset(ring,0,sizeof(ring));
    for(unsigned i=0;i<count;i++) {
        ring[i].id=i; snprintf(ring[i].name,sizeof(ring[i].name),"task%u",i);
        ring[i].state=TASK_READY;ring[i].switches=i*7;
        ring[i].next=&ring[(i+1)%count];
    }
    current=tasks=count?ring:NULL;
}
static void output_reset(void) { console_length=0;console[0]=0; }
static int read_text(const char *path,char *out,u32 capacity) {
    memset(out,0,capacity);int n=vfs_read(path,0,out,capacity-1);assert(n>=0);return n;
}
int main(void) {
    setup_ring(3);ring[1].state=TASK_SLEEPING;
    task_info_t info;
    assert(task_count()==3 && task_snapshot_id(1,&info)==1);
    assert(info.id==1 && !strcmp(info.name,"task1") && info.state==TASK_SLEEPING);
    ring[1].name[0]='X';assert(!strcmp(info.name,"task1"));ring[1].name[0]='t';
    ring[1].state=TASK_DEAD;assert(task_count()==2 && !task_snapshot_id(1,&info));
    assert(task_snapshot_nth(1,&info)==1 && info.id==2 && !task_snapshot_nth(2,&info));
    assert(task_snapshot_nth(0,NULL)==-1 && task_snapshot_id(0,NULL)==-1);
    for(int i=0;i<3;i++)ring[i].state=TASK_DEAD;
    assert(!task_count() && !task_snapshot_nth(0,&info));
    ring[2].next=&ring[1];assert(task_snapshot_id(0,&info)==-1 && !task_count());
    setup_ring(1);ring[0].next=NULL;assert(task_snapshot_nth(0,&info)==-1);
    setup_ring(256);assert(task_count()==256 && task_snapshot_nth(255,&info)==1);
    setup_ring(257);assert(!task_count() && task_snapshot_id(0,&info)==-1);
    setup_ring(0);assert(!task_count() && !task_snapshot_id(0,&info));
    setup_ring(3);ring[1].state=TASK_SLEEPING;

    vfs_init();static const vfs_ops_t empty_ops={0};
    assert(vfs_mount("proc",FS_RAMFS,&empty_ops,NULL,-1,0,0,"fake",1)<0);
    /* Real storage slots remain available, including an unknown partition. */
    for(int i=0;i<VFS_MAX_VOLUMES;i++) {
        char name[16];snprintf(name,sizeof(name),"disk%d",i);
        assert(vfs_mount(name,FS_UNKNOWN,NULL,NULL,-1,0,0,"foreign",0)>=0);
    }
    assert(vfs_volume_count()==VFS_MAX_VOLUMES);
    int previous_disk_ops=disk_ops;
    assert(!procfs_vfs_register());
    u32 registration_epoch=vfs_generation();
    assert(!procfs_vfs_register() && vfs_generation()==registration_epoch);
    assert(disk_ops==previous_disk_ops); /* no fake disk I/O for system mount */
    assert(vfs_volume_count()==VFS_MAX_VOLUMES+1 && vfs_volume(-1)==NULL && !vfs_volume(9));
    vfs_volume_t *proc=vfs_find("proc");assert(proc && proc->disk==-1 && !proc->writable);
    assert(vfs_volume(8)==proc && !strcmp(vfs_kind_name(proc->kind),"procfs"));
    assert(vfs_stat("/proc")==2 && vfs_stat("/mnt/proc/")==2);
    assert(vfs_stat("/proc/0")==2 && vfs_stat("/proc/1/")==2);
    assert(vfs_stat("/proc/1/status")==1 && !vfs_stat("/proc/99/status"));
    assert(!vfs_writable("/proc/meminfo") && !vfs_can_rename("/proc/1/status"));
    assert(vfs_write("/proc/meminfo","x",1)==-2 && vfs_remove("/proc/1/status")==-2);
    assert(vfs_mkdir("/proc/fake")==-2 && vfs_rename("/proc/stat","/proc/foo")==-2);
    assert(vfs_mount("proc",FS_PROC,&empty_ops,NULL,-1,0,0,"duplicate",1)<0);
    assert(vfs_mount("elsewhere",FS_PROC,&empty_ops,NULL,-1,0,0,"bad",0)<0);

    const char *bad[]={"/proc/01/status","/proc/-1/status","/proc/+1/status",
      "/proc/4294967296/status","/proc/99999999999999999999/status","/proc/1/status/",
      "/proc/1/../meminfo","/proc/./cpuinfo","/proc//cpuinfo","/proc/meminfo/x","/proc/self/status","/proc/1/no"};
    char text[PROCFS_TEXT_MAX+1],part[PROCFS_TEXT_MAX+1];
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        assert(!vfs_stat(bad[i]) && !vfs_size(bad[i]));
        assert(vfs_read(bad[i],0,text,sizeof(text))<0);
    }
    /* Canonical UINT32_MAX is valid; UINT32_MAX+1 above was rejected. */
    ring[2].id=0xffffffffu;
    assert(vfs_stat("/proc/4294967295/status")==1);
    read_text("/proc/4294967295/status",text,sizeof(text));assert(strstr(text,"4294967295"));
    ring[2].id=2;
    assert(vfs_readdir("/proc",0x7fffffff,&(vfs_dirent_t){0})==0);
    char overlong[VFS_MAX_PATH];memset(overlong,'x',sizeof(overlong));
    assert(!proc_stat(NULL,overlong) && proc_read(NULL,overlong,0,text,1)<0);
    assert(!proc_stat(NULL,NULL) && proc_read(NULL,NULL,0,text,1)<0);

    /* Bounded random malformed paths through the actual router and parser. */
    u32 seed=0x71ba903u;
    for(u32 i=0;i<20000;i++) {
        char path[VFS_MAX_PATH+8];strcpy(path,"/proc/");
        seed=seed*1664525u+1013904223u;u32 n=seed%VFS_MAX_PATH;
        static const char chars[]="0123456789./+abcdefstatumorin";
        for(u32 j=0;j<n;j++) {seed=seed*1664525u+1013904223u;path[6+j]=chars[seed%(sizeof(chars)-1)];}
        path[6+n]=0;
        int kind=vfs_stat(path);assert(kind>=0 && kind<=2);
        int got=vfs_read(path,seed,text,sizeof(text));assert(got>=-1 && got<(int)sizeof(text));
    }

    const char *files[]={"/proc/cpuinfo","/proc/meminfo","/proc/uptime","/proc/stat","/proc/version","/proc/1/status"};
    for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);i++) {
        const char *path=files[i];int n=read_text(path,text,sizeof(text));
        assert(n>0 && (u32)n==vfs_size(path));
        assert(vfs_read(path,(u32)n,part,1)==0 && vfs_read(path,~0u,part,~0u)==0);
        assert(!vfs_read(path,0,NULL,0) && vfs_read(path,0,NULL,1)<0);
        int off=0;while(off<n) {int got=vfs_read(path,(u32)off,part+off,3);assert(got>0 && got<=3);off+=got;}
        assert(!memcmp(text,part,(size_t)n));
        memset(part,0xa5,sizeof(part));assert(vfs_read(path,0,part,~0u)==n);
        assert(!memcmp(text,part,(size_t)n) && (u8)part[n]==0xa5);
    }
    read_text("/proc/meminfo",text,sizeof(text));
    assert(strstr(text,"MemTotal:") && strstr(text,"262144 kB") && strstr(text,"4096 kB"));
    read_text("/mnt/proc/cpuinfo",text,sizeof(text));assert(strstr(text,"GenuineIntel") && strstr(text,"Test processor"));
    read_text("/proc/1/status",text,sizeof(text));assert(strstr(text,"sleeping") && strstr(text,"task1"));
    memset(ring[1].name,'A',16);ring[1].name[0]='\n';
    read_text("/proc/1/status",text,sizeof(text));assert(strstr(text,"Name: ?AAAA") && !strstr(text,"0x"));
    ring[1].state=TASK_DEAD;assert(!vfs_stat("/proc/1/status") && vfs_read("/proc/1/status",0,text,1)<0);
    ring[1].state=TASK_READY;

    vfs_dirent_t entries[16];
    assert(vfs_readdir_many("/proc",0,entries,16)==8);
    for(int i=0;i<8;i++)assert(entries[i].is_readonly && entries[i].is_system);
    assert(!strcmp(entries[0].name,"cpuinfo") && !strcmp(entries[5].name,"0") && entries[5].is_dir);
    assert(vfs_readdir("/proc/1",0,entries)==1 && !strcmp(entries[0].name,"status"));
    assert(vfs_readdir("/proc/1",1,entries)==0 && vfs_readdir("/proc/meminfo",0,entries)==-1);
    assert(vfs_readdir("/proc/99",0,entries)==-1);
    assert(vfs_read("/proc",0,text,1)<0 && vfs_read("/proc/1",0,text,1)<0);
    assert(!vfs_size("/proc") && !vfs_size("/proc/1"));
    u32 total=1,free_kb=1;vfs_space("/proc",&total,&free_kb);assert(!total && !free_kb);

    /* Stable byte counts while values change, including the high tick word. */
    u32 mem_size=vfs_size("/proc/meminfo"),up_size=vfs_size("/proc/uptime"),stat_size=vfs_size("/proc/stat");
    physical_total=0x100000;physical_used=0xfffff;clock_ticks=0xffffffffffffffffULL;hz=10000;
    assert(vfs_size("/proc/meminfo")==mem_size && vfs_size("/proc/uptime")==up_size && vfs_size("/proc/stat")==stat_size);
    read_text("/proc/uptime",text,sizeof(text));assert(strstr(text,"1844674407370955.16"));
    read_text("/proc/stat",text,sizeof(text));assert(strstr(text,"18446744073709551615"));
    for(u32 i=0;i<10000;i++) {
        u64 value=((u64)(i*2654435761u)<<32)|(i*2246822519u);u32 d=19+i%9982,rem;
        assert(divide(value,d,&rem)==value/d && rem==value%d);
    }
    physical_used=physical_total+1;assert(vfs_read("/proc/meminfo",0,text,sizeof(text))<0);
    physical_used=0;hz=0;assert(vfs_read("/proc/uptime",0,text,1)<0);hz=100;
    u32 epoch=vfs_generation();vfs_init();assert(vfs_generation()!=epoch && vfs_find("proc")==proc);
    assert(vfs_volume_count()==1 && vfs_stat("/proc/version")==1);

    for(int lang=0;lang<2;lang++) {
        kv_current_lang=lang?KV_LANG_RU:KV_LANG_EN;
        output_reset();cmd_cat("/proc/meminfo");assert(strstr(console,"MemTotal:"));
        output_reset();cmd_cat("/proc/0/status");assert(strstr(console,"Pid:") && strstr(console,"task0"));
        output_reset();cmd_cat("hello.txt");assert(strstr(console,"legacy RAM"));
    }
    puts("procfs PASS: real VFS/cat, RO, independent mount slots/rescan, bounded paths and task copies, partial reads, stable widths, 64-bit ticks, task disappearance and malformed rings");
}
