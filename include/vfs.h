/* ============================================================
 *  KvantOS - VFS: a single namespace over several filesystems
 *
 *  Until now the system knew exactly two storages, each with its own
 *  API: ramfs (files in RAM) and KvFS (our own on-disk format). Adding
 *  FAT32 and NTFS that way would mean four incompatible interfaces and
 *  a file manager full of switch statements.
 *
 *  So every filesystem now implements one and the same set of
 *  operations, and the rest of the system talks only to the VFS.
 *
 *  Paths look like /mnt/<volume>/dir/file. The first component selects
 *  the mounted volume, the rest is passed to its driver.
 * ============================================================ */
#ifndef KV_VFS_H
#define KV_VFS_H

#define VFS_MAX_VOLUMES   8
#define VFS_MAX_PATH      256
#define VFS_MAX_NAME      128
#define VFS_LABEL_MAX     32

/* Filesystem kinds the system can recognise. */
typedef enum {
    FS_NONE = 0,
    FS_RAMFS,
    FS_KVFS,
    FS_FAT16,
    FS_FAT32,
    FS_NTFS,
    FS_UNKNOWN        /* a partition exists but the format is foreign */
} fs_kind_t;

/* One directory entry as reported by a driver. */
typedef struct {
    char name[VFS_MAX_NAME];
    u32  size;            /* bytes; 0 for directories        */
    u8   is_dir;
    u8   is_readonly;
    u8   is_hidden;
    u8   is_system;
    u16  mtime_year;      /* 0 when the driver has no timestamps */
    u8   mtime_month, mtime_day, mtime_hour, mtime_min;
} vfs_dirent_t;

/* Operations a filesystem driver must provide. Anything it cannot do
   is left NULL and the VFS reports "not supported" instead of crashing:
   NTFS, for instance, is deliberately read-only here. */
typedef struct vfs_ops {
    /* List entry: 1 filled, 0 clean EOF, -1 unavailable/invalid/read error. */
    int (*readdir)(void *fs, const char *path, int index, vfs_dirent_t *out);
    /* Read up to `max` bytes of `path` starting at `off`. Returns count. */
    int (*read)(void *fs, const char *path, u32 off, void *buf, u32 max);
    /* Full size of a file, 0 when missing. */
    u32 (*size)(void *fs, const char *path);
    /* 1 = exists, 2 = exists and is a directory, 0 = missing. */
    int (*stat)(void *fs, const char *path);
    /* Optional writing side. */
    int (*write)(void *fs, const char *path, const void *data, u32 size);
    int (*remove)(void *fs, const char *path);
    int (*mkdir)(void *fs, const char *path);
    /* Free and total space in KiB. */
    void (*space)(void *fs, u32 *total_kb, u32 *free_kb);
    /* Optional bounded batch listing; count 1..128, returns count or -1. */
    int (*readdir_many)(void *fs,const char *path,int first,vfs_dirent_t *out,int count);
    /* Optional no-replace same-volume file rename. Never copy-delete fallback. */
    int (*rename)(void *fs,const char *from,const char *to);
} vfs_ops_t;

/* A mounted volume. */
typedef struct {
    char           name[16];        /* what appears in /mnt/<name>  */
    char           label[VFS_LABEL_MAX];
    fs_kind_t      kind;
    const vfs_ops_t *ops;
    void          *fs;              /* driver private data          */
    int            disk;            /* block ID, -1 for RAM        */
    u32            lba_start;       /* first sector of the partition */
    u32            lba_count;
    u8             writable;
    u8             used;
} vfs_volume_t;

/* ---- volume management ---- */
void         vfs_init(void);
u32          vfs_generation(void); /* Namespace reset or native remount/fence. */
void         vfs_namespace_changed(void); /* Bump only; no I/O, rescan or reset. */
int          vfs_mount(const char *name, fs_kind_t kind, const vfs_ops_t *ops,
                       void *fs, int disk, u32 lba_start, u32 lba_count,
                       const char *label, int writable);
int          vfs_volume_count(void);
vfs_volume_t *vfs_volume(int i);
vfs_volume_t *vfs_find(const char *name);
const char  *vfs_kind_name(fs_kind_t k);

/* Scan ATA and USB disks, read its partition table and mount what we
   understand. Returns the number of volumes mounted. */
int          vfs_autoscan(void);
const char  *vfs_scan_status(void);

/* ---- path operations (the same for every filesystem) ---- */
int  vfs_readdir(const char *path, int index, vfs_dirent_t *out);
int  vfs_readdir_many(const char *path,int first,vfs_dirent_t *out,int count);
int  vfs_read(const char *path, u32 off, void *buf, u32 max);
u32  vfs_size(const char *path);
int  vfs_stat(const char *path);
int  vfs_write(const char *path, const void *data, u32 size);
int  vfs_remove(const char *path);
/* rename:0 success,-1 unavailable,-2 RO/unsupported,-3 missing,-4 bad name,
   -5 target exists,-6 outcome uncertain (remount),-7 cross-volume unsupported. */
int  vfs_rename(const char *from,const char *to);
int  vfs_can_rename(const char *path);
int  vfs_mkdir(const char *path);
void vfs_space(const char *path, u32 *total_kb, u32 *free_kb);
int  vfs_writable(const char *path);

/* Helpers shared by the shell and the file manager. */
void vfs_join(char *dst, u32 dstsz, const char *dir, const char *name);
void vfs_parent(char *dst, u32 dstsz, const char *path);
int  vfs_is_root(const char *path);

/* blk_read/blk_write return 0 on success and -1 on failure, which reads
   backwards in the filesystem code. These wrappers return 1 on success
   so the drivers can say "if (!disk_read(...)) fail". */
static inline int disk_read(int disk, u32 lba, u8 count, void *buf) {
    return blk_read(disk, lba, count, buf) == 0;
}
static inline int disk_write(int disk, u32 lba, u8 count, const void *buf) {
    return blk_write(disk, lba, count, buf) == 0;
}

/* A successful write alone is not a persistence barrier on USB. */
static inline int disk_sync(int disk) { return blk_sync(disk)==0; }

/* ---- individual drivers ---- */
void fat_reset_mounts(void);
void ntfs_reset_mounts(void);
void ramfs_vfs_register(void);
void kvfs_vfs_register(void);
int  fat_probe(int disk, u32 lba_start, u32 lba_count, char *label, fs_kind_t *kind);
/* Explicit opt-in; normal FAT mounts are RO, only owned/recovered FAT32 may RW. */
int  fat_write_mode(const char *name,int enable);
int  fat_mount(const char *name, int disk, u32 lba_start, u32 lba_count);
int  ntfs_probe(int disk, u32 lba_start, u32 lba_count, char *label);
int  ntfs_mount(const char *name, int disk, u32 lba_start, u32 lba_count);

#endif
