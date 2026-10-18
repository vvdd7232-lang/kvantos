/* ============================================================
 *  KvantOS - KvFS, a simple on-disk filesystem
 *
 *  Deliberately primitive so that it can be read end to end in one
 *  sitting and understood without documentation:
 *
 *    sector 0            - superblock (signature, counters)
 *    sectors 1..8        - directory: 64 entries of 64 bytes
 *    sectors 9..         - data area, handed out in chunks
 *
 *  A file occupies a contiguous chain of sectors. There is no
 *  fragmentation: deleting leaves a hole and a new file is placed by
 *  the "first suitable gap" rule. For a system whose files are a dozen
 *  applications and notes that is more than enough.
 *
 *  All numbers are little-endian, exactly as the CPU stores them.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"
#include "native_name.h"

#define KVFS_MAGIC     0x5346564Bu     /* "KVFS" */
#define KVFS_VERSION   2
#define SECTOR_SIZE    512

/* The first megabyte of the disk is NOT ours: the bootloader lives
   there. Sector 0 is the MBR, sectors 1..2047 hold the body of GRUB
   (core.img). Ordinary systems do exactly the same, starting their
   first partition at the 1 MiB mark. Everything below is counted from
   this base. */
#define KVFS_BASE      2048

#define DIR_SECTOR     (KVFS_BASE + 1)
#define DIR_SECTORS    8
#define DATA_START     (DIR_SECTOR + DIR_SECTORS)
#define KVFS_MAX_FILES 64

/* A directory entry is exactly 64 bytes, eight per sector */
typedef struct {
    char name[40];      /* name, zero padded        */
    u32  start;         /* first data sector        */
    u32  sectors;       /* how many sectors are used */
    u32  size;          /* actual size in bytes     */
    u32  flags;         /* 1 - entry in use, 2 - executable file */
    u32  reserved[2];   /* pad to a round 64 bytes: 8 entries per sector */
} kvfs_dirent_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 total_sectors;
    u32 data_start;
    u32 file_count;
    u32 reserved[3];
} kvfs_super_t;

/* A build-time check: a directory entry must be exactly 64 bytes,
   otherwise images built by sdk/mkdisk.py would be read misaligned. */
_Static_assert(sizeof(kvfs_dirent_t) == 64, "a KvFS directory entry must be 64 bytes");

static kvfs_super_t   super;
static kvfs_dirent_t  dir[KVFS_MAX_FILES];
static int mounted = 0;
static int dev     = -1;         /* disk index inside the ATA driver */

/* A one-sector buffer: keeping 512 bytes on the stack is unwise,
   a task stack is only 8 KiB. */
static u8 secbuf[SECTOR_SIZE];

int kvfs_mounted(void) { return mounted; }

static int read_dir(void) {
    for (int s = 0; s < DIR_SECTORS; s++) {
        if (ata_read(dev, DIR_SECTOR + s, 1, secbuf) < 0) return -1;
        memcpy((u8 *)dir + s * SECTOR_SIZE, secbuf, SECTOR_SIZE);
    }
    return 0;
}

static int write_dir(void) {
    for (int s = 0; s < DIR_SECTORS; s++) {
        memcpy(secbuf, (u8 *)dir + s * SECTOR_SIZE, SECTOR_SIZE);
        if (ata_write(dev, DIR_SECTOR + s, 1, secbuf) < 0) return -1;
    }
    return 0;
}

static int write_super(void) {
    memset(secbuf, 0, SECTOR_SIZE);
    memcpy(secbuf, &super, sizeof(super));
    return ata_write(dev, KVFS_BASE, 1, secbuf);
}

/* Mount an existing filesystem. Returns 0 when the disk holds KvFS. */
int kvfs_mount(void) {
    vfs_namespace_changed();
    mounted = 0;
    dev = ata_boot_drive();
    if (dev < 0) return -1;

    if (ata_read(dev, KVFS_BASE, 1, secbuf) < 0) return -2;
    memcpy(&super, secbuf, sizeof(super));
    if (super.magic != KVFS_MAGIC) return -3;      /* a foreign or empty disk */
    if (super.version != KVFS_VERSION) return -4;
    if (super.data_start!=DATA_START || super.total_sectors<DATA_START+64 ||
        super.total_sectors>ata_sectors(dev)) return -4;
    if (read_dir() < 0) return -2;
    /* Reject bad extents before first-fit arithmetic or string lookup. */
    for(int i=0;i<KVFS_MAX_FILES;i++) {
        kvfs_dirent_t *e=&dir[i];
        if(!(e->flags&1)) continue;
        int terminated=0;
        for(unsigned j=0;j<sizeof(e->name);j++) if(!e->name[j]) { terminated=1; break; }
        if(!terminated || !e->name[0] || (e->flags&~3u) || !e->sectors ||
           e->start<DATA_START || e->start>=super.total_sectors ||
           e->sectors>super.total_sectors-e->start ||
           e->size/512+(e->size%512!=0)>e->sectors || e->size>0x7fffffffu) return -4;
        for(int j=0;j<i;j++) if(dir[j].flags&1) {
            if(!strcmp(e->name,dir[j].name) ||
               (e->start<dir[j].start+dir[j].sectors && dir[j].start<e->start+e->sectors)) return -4;
        }
    }
    /* v2 file_count is advisory; directory entries are authoritative. */
    super.file_count=0;
    for(int i=0;i<KVFS_MAX_FILES;i++) if(dir[i].flags&1) super.file_count++;

    mounted = 1;
    return 0;
}

/* Create a new filesystem over the disk. Wipes the directory but not
   the data: the sectors holding file bodies simply stop being
   referenced by anyone. */
int kvfs_format(void) {
    vfs_namespace_changed();
    mounted=0;
    dev = ata_boot_drive();
    if (dev < 0) return -1;

    u32 total = ata_sectors(dev);
    if (total < DATA_START + 64) return -5;       /* the disk is indecently small */

    memset(&super, 0, sizeof(super));
    super.magic         = KVFS_MAGIC;
    super.version       = KVFS_VERSION;
    super.total_sectors = total;
    super.data_start    = DATA_START;
    super.file_count    = 0;

    memset(dir, 0, sizeof(dir));
    if (write_super() < 0) return -2;
    if (write_dir() < 0)   return -2;

    mounted = 1;
    return 0;
}

static int find_entry(const char *name) {
    for (int i = 0; i < KVFS_MAX_FILES; i++)
        if ((dir[i].flags & 1) && !strcmp(dir[i].name, name)) return i;
    return -1;
}

/* Find a contiguous gap of the required length, first-fit style.
   Used regions are not kept sorted, so we walk the sectors and check
   every candidate for overlaps. */
static u32 find_space(u32 need_sectors) {
    u32 candidate = super.data_start;
    u32 limit = super.total_sectors;
    if(candidate>limit || need_sectors>limit-candidate) return 0;

    for (int guard = 0; guard < KVFS_MAX_FILES + 1; guard++) {
        u32 next = 0;
        int clash = 0;
        for (int i = 0; i < KVFS_MAX_FILES; i++) {
            if (!(dir[i].flags & 1)) continue;
            u32 a = dir[i].start, b = dir[i].start + dir[i].sectors;
            /* does [candidate, candidate+need) overlap [a, b)? */
            if (candidate < b && a < candidate + need_sectors) {
                clash = 1;
                if (b > next) next = b;
            }
        }
        if (!clash) {
            if (candidate>limit || need_sectors>limit-candidate) return 0;   /* no room */
            return candidate;
        }
        candidate = next;
        if (candidate>limit || need_sectors>limit-candidate) return 0;
    }
    return 0;
}

int kvfs_file_count(void) {
    if (!mounted) return 0;
    int n = 0;
    for (int i = 0; i < KVFS_MAX_FILES; i++) if (dir[i].flags & 1) n++;
    return n;
}

/* Iterate over files: fills in the name, size and executable flag.
   Returns 0 when an entry with that ordinal exists. */
int kvfs_list(int index, char *name, u32 *size, int *is_exec) {
    if (!mounted) return -1;
    int n = 0;
    for (int i = 0; i < KVFS_MAX_FILES; i++) {
        if (!(dir[i].flags & 1)) continue;
        if (n == index) {
            if (name) strncpy(name, dir[i].name, 40);
            if (size) *size = dir[i].size;
            if (is_exec) *is_exec = (dir[i].flags & 2) ? 1 : 0;
            return 0;
        }
        n++;
    }
    return -1;
}

int kvfs_exists(const char *name) {
    if (!mounted) return 0;
    return mounted && find_entry(name) >= 0;
}

u32 kvfs_size(const char *name) {
    if (!mounted) return 0;
    int i = find_entry(name);
    return i < 0 ? 0 : dir[i].size;
}

/* Publish just the affected directory sector, after the new body is durable.
 * No later superblock write may turn a committed save into a reported failure.
 * v2's on-disk file_count is advisory, recalculated on mount by all our readers.
 * A failed metadata write has an unknown outcome: fence the mount rather than
 * let a later save flush stale RAM metadata over it. No torn-sector guarantee. */
static int commit_entry(int slot,const kvfs_dirent_t *entry) {
    unsigned group=(unsigned)slot/8;
    memcpy(secbuf,(u8 *)dir+group*512,512);
    memcpy(secbuf+(slot%8)*sizeof(*entry),entry,sizeof(*entry));
    if(ata_write(dev,DIR_SECTOR+group,1,secbuf)<0) { mounted=0;vfs_namespace_changed();return -2; }
    dir[slot]=*entry;
    super.file_count=(u32)kvfs_file_count();
    return 0;
}
/* Copy-on-write: never release/reuse the old extent before publication.
 * Even a shorter replacement requires spare contiguous space. */
int kvfs_write(const char *name,const void *data,u32 size,int is_exec) {
    if(!mounted) return -1;
    if(!name || !name[0]) return -4;
    if(strlen(name)>=40) return -5;
    if((size && !data) || size>0x7fffffffu) return -8;
    u32 need=size/512+(size%512!=0);
    if(!need) need=1;
    int slot=find_entry(name);
    if(slot<0) for(int i=0;i<KVFS_MAX_FILES;i++) if(!(dir[i].flags&1)) { slot=i; break; }
    if(slot<0) return -6;
    u32 start=find_space(need);
    if(!start) return -7;
    kvfs_dirent_t next;
    memset(&next,0,sizeof(next)); strncpy(next.name,name,sizeof(next.name));
    next.start=start; next.sectors=need; next.size=size; next.flags=1|(is_exec?2:0);
    for(u32 s=0;s<need;s++) {
        u32 at=s*512,bytes=size>at?size-at:0;
        if(bytes>512) bytes=512;
        memset(secbuf,0,512);
        if(bytes) memcpy(secbuf,(const u8 *)data+at,bytes);
        if(ata_write(dev,start+s,1,secbuf)<0) return -2;
    }
    return commit_entry(slot,&next);
}

/* Read a file into a buffer. Returns the number of bytes read, or < 0. */
int kvfs_read(const char *name, void *buf, u32 max) {
    if (!mounted) return -1;
    int i = find_entry(name);
    if (i < 0) return -3;

    u32 size = dir[i].size;
    if (size > max) size = max;
    u32 need = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    u8 *p = (u8 *)buf;
    for (u32 s = 0; s < need; s++) {
        if (ata_read(dev, dir[i].start + s, 1, secbuf) < 0) return -2;
        u32 chunk = size - s * SECTOR_SIZE;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        memcpy(p + s * SECTOR_SIZE, secbuf, chunk);
    }
    return (int)size;
}

int kvfs_delete(const char *name) {
    if (!mounted) return -1;
    int i = find_entry(name);
    if (i < 0) return -3;
    kvfs_dirent_t next=dir[i]; next.flags=0;
    return commit_entry(i,&next);
}

/* Rename one existing directory entry, retaining its extent/size/flags.
   A failed metadata write is uncertain, and commit_entry fences the mount. */
int kvfs_rename(const char *from,const char *to) {
    if(!mounted)return -1;
    int length=native_name_length(to,sizeof(dir[0].name));
    if(length<0 || native_name_length(from,sizeof(dir[0].name))<0)return -4;
    int slot=find_entry(from);if(slot<0)return -3;
    if(!strcmp(from,to))return 0;
    if(find_entry(to)>=0)return -5;
    kvfs_dirent_t next=dir[slot];memset(next.name,0,sizeof(next.name));memcpy(next.name,to,(u32)length);
    return commit_entry(slot,&next)?-6:0;
}

/* Usage information for the df command and the installer window */
void kvfs_stats(u32 *total_mb, u32 *used_kb, u32 *files) {
    if (total_mb) *total_mb = mounted ? (super.total_sectors >> 11) : 0;
    if (files)    *files    = mounted ? (u32)kvfs_file_count() : 0;
    if (used_kb) {
        u32 sec = 0;
        if (mounted)
            for (int i = 0; i < KVFS_MAX_FILES; i++)
                if (dir[i].flags & 1) sec += dir[i].sectors;
        *used_kb = sec / 2;              /* 512 bytes = half a kilobyte */
    }
}

const char *kvfs_error(int code) {
    switch (code) {
        case  0: return T("success", "успешно");
        case -1: return T("filesystem is not mounted", "файловая система не подключена");
        case -2: return T("disk I/O error", "ошибка ввода-вывода диска");
        case -3: return T("file not found", "файл не найден");
        case -4: return T("empty file name", "пустое имя файла");
        case -5: return T("name longer than 39 characters", "имя длиннее 39 символов");
        case -6: return T("directory is full (64 files maximum)", "каталог переполнен (максимум 64 файла)");
        case -7: return T("not enough spare contiguous space (safe replacement)", "не хватает свободного непрерывного места для безопасной замены");
        case -8: return T("invalid data buffer or file size", "неверный буфер данных или размер файла");
        default: return T("unknown error", "неизвестная ошибка");
    }
}
