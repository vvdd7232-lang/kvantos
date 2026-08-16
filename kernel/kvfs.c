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
    mounted = 0;
    dev = ata_boot_drive();
    if (dev < 0) return -1;

    if (ata_read(dev, KVFS_BASE, 1, secbuf) < 0) return -2;
    memcpy(&super, secbuf, sizeof(super));
    if (super.magic != KVFS_MAGIC) return -3;      /* a foreign or empty disk */
    if (super.version != KVFS_VERSION) return -4;
    if (read_dir() < 0) return -2;

    mounted = 1;
    return 0;
}

/* Create a new filesystem over the disk. Wipes the directory but not
   the data: the sectors holding file bodies simply stop being
   referenced by anyone. */
int kvfs_format(void) {
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
            if (candidate + need_sectors > limit) return 0;   /* no room */
            return candidate;
        }
        candidate = next;
        if (candidate + need_sectors > limit) return 0;
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
    return find_entry(name) >= 0;
}

u32 kvfs_size(const char *name) {
    if (!mounted) return 0;
    int i = find_entry(name);
    return i < 0 ? 0 : dir[i].size;
}

/* Write a whole file. An existing file with the same name is replaced. */
int kvfs_write(const char *name, const void *data, u32 size, int is_exec) {
    if (!mounted) return -1;
    if (!name || !name[0]) return -4;
    if (strlen(name) >= 40) return -5;

    u32 need = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (need == 0) need = 1;

    int slot = find_entry(name);
    if (slot >= 0) {
        /* Rewrite: if the new file fits in the old place, reuse it */
        if (need <= dir[slot].sectors) {
            dir[slot].size  = size;
            dir[slot].flags = 1 | (is_exec ? 2 : 0);
        } else {
            dir[slot].flags = 0;          /* release it and search again */
            slot = -1;
        }
    }

    if (slot < 0) {
        for (int i = 0; i < KVFS_MAX_FILES; i++)
            if (!(dir[i].flags & 1)) { slot = i; break; }
        if (slot < 0) return -6;          /* the directory is full */

        u32 start = find_space(need);
        if (!start) return -7;            /* no contiguous space */

        memset(&dir[slot], 0, sizeof(dir[slot]));
        strncpy(dir[slot].name, name, 40);
        dir[slot].name[39] = 0;
        dir[slot].start   = start;
        dir[slot].sectors = need;
        dir[slot].size    = size;
        dir[slot].flags   = 1 | (is_exec ? 2 : 0);
    }

    /* Written sector by sector: the last one is zero padded */
    const u8 *p = (const u8 *)data;
    for (u32 s = 0; s < need; s++) {
        u32 chunk = size - s * SECTOR_SIZE;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        memset(secbuf, 0, SECTOR_SIZE);
        if (chunk > 0 && chunk <= SECTOR_SIZE) memcpy(secbuf, p + s * SECTOR_SIZE, chunk);
        if (ata_write(dev, dir[slot].start + s, 1, secbuf) < 0) return -2;
    }

    super.file_count = (u32)kvfs_file_count();
    if (write_dir() < 0) return -2;
    if (write_super() < 0) return -2;
    return 0;
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
    dir[i].flags = 0;
    super.file_count = (u32)kvfs_file_count();
    if (write_dir() < 0) return -2;
    if (write_super() < 0) return -2;
    return 0;
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
        case -7: return T("not enough contiguous space on the disk", "не хватает непрерывного места на диске");
        default: return T("unknown error", "неизвестная ошибка");
    }
}
