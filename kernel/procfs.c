/* KvantOS read-only telemetry filesystem.
 * Independently implemented for the stateless KvantOS VFS. Design reference:
 * ToaruOS kernel/vfs/procfs.c, 60b54503807a38ae1f0692d44cf8b569301febbc.
 * No ToaruOS source is incorporated; see docs/PROCFS-SHM-MODULES.ru.md.
 */
#include "kernel.h"
#include "vfs.h"
#include "procfs.h"

enum { P_ROOT, P_TASK, P_CPU, P_MEM, P_UPTIME, P_STAT, P_VERSION, P_STATUS };
static const char *const proc_files[] = {"cpuinfo", "meminfo", "uptime", "stat", "version"};
#define PROC_FILES ((int)(sizeof(proc_files)/sizeof(proc_files[0])))
typedef struct { int kind; u32 pid; } proc_node;
typedef struct { char data[PROCFS_TEXT_MAX]; u32 used; int failed; } proc_text;

#ifdef KV_PROCFS_HOSTTEST
static kv_flags_t proc_lock(void) { return 0; }
static void proc_unlock(kv_flags_t flags) { (void)flags; }
#else
static kv_flags_t proc_lock(void) { return irq_save(); }
static void proc_unlock(kv_flags_t flags) { irq_restore(flags); }
#endif

/* Canonical paths only; no truncation aliases, dot components or PID wrap. */
static int proc_path(const char *path, proc_node *node) {
    if (!path || !node) return 0;
    u32 length = 0;
    while (length < VFS_MAX_PATH && path[length]) length++;
    if (!length || length == VFS_MAX_PATH || path[0] != '/') return 0;
    if (!path[1]) { *node = (proc_node){P_ROOT,0}; return 1; }
    for (int i=0; i<PROC_FILES; i++)
        if (!strcmp(path+1,proc_files[i])) { *node=(proc_node){P_CPU+i,0}; return 1; }
    const char *p=path+1;
    if (*p<'0' || *p>'9' || (*p=='0' && p[1]>='0' && p[1]<='9')) return 0;
    u32 pid=0;
    do {
        u32 digit=(u32)(*p++-'0');
        if (pid>(0xffffffffu-digit)/10u) return 0;
        pid=pid*10u+digit;
    } while (*p>='0' && *p<='9');
    if (!*p || (*p=='/' && !p[1])) *node=(proc_node){P_TASK,pid};
    else if (!strcmp(p,"/status")) *node=(proc_node){P_STATUS,pid};
    else return 0;
    return 1;
}

static void append(proc_text *text,const char *fmt,...) {
    if (text->failed) return;
    u32 left=PROCFS_TEXT_MAX-text->used;
    va_list args; va_start(args,fmt);
    kvsnprintf_v(text->data+text->used,left,fmt,args);
    va_end(args);
    u32 n=(u32)strlen(text->data+text->used);
    if (n>=left-1) { text->failed=1; return; }
    text->used+=n;
}

/* Fixed BYTE widths keep VFS size/read and partial-read offsets stable while
   values change. Padding is spaces, never octal-looking leading zeroes.
   Non-ASCII/control bytes in kernel-supplied identifiers become '?'. */
static void field(proc_text *text,const char *key,const char *value,u32 width) {
    char clean[64]; u32 i=0;
    if (width>=sizeof(clean)) { text->failed=1; return; }
    int ended=0;
    for (;i<width;i++) {
        u8 c=ended?0:(u8)*value;
        if (!c) ended=1;
        else value++;
        clean[i]=ended?' ':(c>=32 && c<127?(char)c:'?');
    }
    clean[i]=0;
    append(text,"%s%s\n",key,clean);
}

/* Full 64-bit tick/uptime formatting without libgcc __udivdi3 on i386.
   All divisors here are <=10000, so the 32-bit remainder cannot overflow. */
static u64 divide(u64 value,u32 divisor,u32 *remainder) {
    u64 result=0; u32 rem=0;
    for (int bit=63;bit>=0;bit--) {
        rem=rem*2u+(u32)((value>>bit)&1u);
        if (rem>=divisor) { rem-=divisor; result|=(u64)1<<bit; }
    }
    if (remainder) *remainder=rem;
    return result;
}

static void decimal64(char out[21],u64 value) {
    for (int i=19;i>=0;i--) {
        u32 digit; value=divide(value,10,&digit); out[i]=(char)('0'+digit);
    }
    out[20]=0;
    for (int i=0;i<19 && out[i]=='0';i++) out[i]=' ';
}

/* One independent snapshot per call, not an open-file/session snapshot. */
static int proc_generate(proc_node node,proc_text *text) {
    memset(text,0,sizeof(*text));
    switch (node.kind) {
        case P_CPU: {
            char vendor[16]={0},brand[128]={0};
            cpu_vendor(vendor); cpu_brand(brand);
            append(text,"Architecture: %s\n",KV_ARCH);
            field(text,"Vendor: ",vendor,12);
            field(text,"Model: ",brand,48);
            break;
        }
        case P_MEM: {
            kv_flags_t flags=proc_lock();
            u32 total=pmm_total_frames(),used=pmm_used_frames();
            proc_unlock(flags);
            /* PMM represents at most 4GiB in this kernel. Fail closed on
               inconsistent counters rather than wrapping negative free. */
            if (total>0x100000u || used>total) return -1;
            append(text,"MemTotal: %10u kB\nMemUsed:  %10u kB\nMemFree:  %10u kB\n",
                   total*4u,used*4u,(total-used)*4u);
            break;
        }
        case P_UPTIME:
        case P_STAT: {
            kv_flags_t flags=proc_lock();
            u64 ticks=timer_ticks(); u32 hz=timer_hz();
            u32 tasks=task_count();
            proc_unlock(flags);
            if (hz<19 || hz>10000) return -1;
            char number[21]; u32 remainder=0;
            if (node.kind==P_UPTIME) {
                decimal64(number,divide(ticks,hz,&remainder));
                append(text,"%s.%02u\n",number,remainder*100u/hz);
            } else {
                decimal64(number,ticks);
                append(text,"Tasks: %10u\nTimerHz: %10u\nTicks: %s\n",tasks,hz,number);
            }
            break;
        }
        case P_VERSION:
            append(text,"%s %s (%s)\n",KV_NAME,KV_VERSION,KV_ARCH);
            break;
        case P_STATUS: {
            task_info_t task;
            if (task_snapshot_id(node.pid,&task)!=1) return -1;
            append(text,"Pid: %10u\n",task.id);
            field(text,"Name: ",task.name,16);
            const char *state=task.state==TASK_READY?"ready":
                              task.state==TASK_SLEEPING?"sleeping":"unknown";
            field(text,"State: ",state,8);
            append(text,"Switches: %10u\n",task.switches);
            break;
        }
        default: return -1;
    }
    return text->failed?-1:(int)text->used;
}

static int proc_stat(void *fs,const char *path) {
    (void)fs; proc_node node;
    if (!proc_path(path,&node)) return 0;
    if (node.kind==P_ROOT) return 2;
    if (node.kind==P_TASK || node.kind==P_STATUS) {
        task_info_t task;
        if (task_snapshot_id(node.pid,&task)!=1) return 0;
        return node.kind==P_TASK?2:1;
    }
    return 1;
}

static int proc_read(void *fs,const char *path,u32 offset,void *buf,u32 max) {
    (void)fs; proc_node node; proc_text text;
    if ((!buf && max) || !proc_path(path,&node)) return -1;
    int length=proc_generate(node,&text);
    if (length<0) return -1;
    if (offset>=(u32)length || !max) return 0;
    u32 count=(u32)length-offset;
    if (count>max) count=max;
    memcpy(buf,text.data+offset,count);
    return (int)count;
}

static u32 proc_size(void *fs,const char *path) {
    (void)fs; proc_node node; proc_text text;
    if (!proc_path(path,&node)) return 0;
    int length=proc_generate(node,&text);
    return length<0?0:(u32)length;
}

static int proc_readdir(void *fs,const char *path,int index,vfs_dirent_t *out) {
    (void)fs; proc_node node;
    if (!out || index<0 || !proc_path(path,&node)) return -1;
    const char *name=NULL; char pid[12]; u32 bytes=0; int dir=0;
    if (node.kind==P_ROOT) {
        if (index<PROC_FILES) {
            name=proc_files[index];
            proc_node file={P_CPU+index,0}; proc_text text;
            int length=proc_generate(file,&text);
            if (length<0) return -1;
            bytes=(u32)length;
        } else {
            task_info_t task;
            int found=task_snapshot_nth((u32)(index-PROC_FILES),&task);
            if (found!=1) return found;
            ksnprintf(pid,sizeof(pid),"%u",task.id); name=pid; dir=1;
        }
    } else if (node.kind==P_TASK) {
        task_info_t task;
        if (task_snapshot_id(node.pid,&task)!=1) return -1;
        if (index) return 0;
        name="status";
        char status[32]; ksnprintf(status,sizeof(status),"/%u/status",node.pid);
        bytes=proc_size(NULL,status);
        if (!bytes) return -1;
    } else return -1;
    memset(out,0,sizeof(*out));
    strncpy(out->name,name,sizeof(out->name));
    out->size=bytes; out->is_dir=(u8)dir; out->is_readonly=out->is_system=1;
    return 1;
}

static const vfs_ops_t proc_ops={
    .readdir=proc_readdir,.read=proc_read,.size=proc_size,.stat=proc_stat
};

int procfs_vfs_register(void) {
    vfs_volume_t *existing=vfs_find("proc");
    if (existing) return existing->kind==FS_PROC && existing->ops==&proc_ops?0:-1;
    return vfs_mount("proc",FS_PROC,&proc_ops,NULL,-1,0,0,"System information",0)<0?-1:0;
}
