/* ============================================================
 *  KvantOS - VFS adapters for the two native storages
 *
 *  ramfs and KvFS predate the VFS and both keep a flat list of files
 *  with no directories. Wrapping them here means the file manager and
 *  the shell treat them exactly like a FAT or NTFS volume, and the old
 *  APIs keep working for the code that already uses them.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"

/* ---------------- ramfs ---------------- */

static int rfs_readdir(void *fs, const char *path, int index, vfs_dirent_t *out) {
    (void)fs;
    /* flat storage: only the root has entries */
    if (path[0] != '/' || path[1] != 0) return 0;

    rfile_t *tbl = ramfs_table();
    int n = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!tbl[i].used) continue;
        if (n == index) {
            memset(out, 0, sizeof(*out));
            strncpy(out->name, tbl[i].name, VFS_MAX_NAME);
            out->size = tbl[i].size;
            return 1;
        }
        n++;
    }
    return 0;
}

static const char *strip(const char *path) {
    while (*path == '/') path++;
    return path;
}

static int rfs_stat(void *fs, const char *path) {
    (void)fs;
    if (path[0] == '/' && path[1] == 0) return 2;
    return ramfs_find(strip(path)) ? 1 : 0;
}

static u32 rfs_size(void *fs, const char *path) {
    (void)fs;
    rfile_t *f = ramfs_find(strip(path));
    return f ? f->size : 0;
}

static int rfs_read(void *fs, const char *path, u32 off, void *buf, u32 max) {
    (void)fs;
    rfile_t *f = ramfs_find(strip(path));
    if (!f) return -1;
    if (off >= f->size) return 0;
    u32 n = f->size - off;
    if (n > max) n = max;
    memcpy(buf, f->data + off, n);
    return (int)n;
}

static int rfs_write(void *fs, const char *path, const void *data, u32 size) {
    (void)fs;
    const char *name = strip(path);
    if (ramfs_find(name)) ramfs_delete(name);
    return ramfs_create(name, (const char *)data, size) ? (int)size : -1;
}

static int rfs_remove(void *fs, const char *path) {
    (void)fs;
    return ramfs_delete(strip(path)) ? 0 : -1;
}

static void rfs_space(void *fs, u32 *total_kb, u32 *free_kb) {
    (void)fs;
    rfile_t *tbl = ramfs_table();
    u32 used = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) if (tbl[i].used) used += tbl[i].size;
    /* ramfs lives in the kernel heap, so "total" is what the heap has */
    u32 htotal = 0, hused = 0, hblocks = 0;
    heap_stats(&htotal, &hused, &hblocks);
    u32 heap_kb = htotal / 1024;
    if (total_kb) *total_kb = heap_kb;
    if (free_kb)  *free_kb  = heap_kb - used / 1024;
}

static const vfs_ops_t ramfs_ops = {
    rfs_readdir, rfs_read, rfs_size, rfs_stat,
    rfs_write, rfs_remove, NULL,          /* no directories in ramfs */
    rfs_space
};

void ramfs_vfs_register(void) {
    vfs_mount("ram", FS_RAMFS, &ramfs_ops, NULL, -1, 0, 0, "RAM disk", 1);
}

/* ---------------- KvFS ---------------- */

static int kfs_readdir(void *fs, const char *path, int index, vfs_dirent_t *out) {
    (void)fs;
    if (path[0] != '/' || path[1] != 0) return 0;

    char name[64];
    u32  size;
    int  is_exec;
    if (!kvfs_list(index, name, &size, &is_exec)) return 0;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, name, VFS_MAX_NAME);
    out->size = size;
    return 1;
}

static int kfs_stat(void *fs, const char *path) {
    (void)fs;
    if (path[0] == '/' && path[1] == 0) return 2;
    return kvfs_exists(strip(path)) ? 1 : 0;
}

static u32 kfs_size(void *fs, const char *path) {
    (void)fs;
    return kvfs_size(strip(path));
}

static int kfs_read(void *fs, const char *path, u32 off, void *buf, u32 max) {
    (void)fs;
    const char *name = strip(path);
    u32 total = kvfs_size(name);
    if (!total) return -1;
    if (off >= total) return 0;

    /* KvFS reads whole files, so a partial read goes through a bounce
       buffer. Files here are small (the installer's payload). */
    u8 *tmp = (u8 *)kmalloc(total);
    if (!tmp) return -1;
    int r = kvfs_read(name, tmp, total);
    if (r < 0) { kfree(tmp); return -1; }

    u32 n = total - off;
    if (n > max) n = max;
    memcpy(buf, tmp + off, n);
    kfree(tmp);
    return (int)n;
}

static int kfs_write(void *fs, const char *path, const void *data, u32 size) {
    (void)fs;
    const char *name = strip(path);
    int is_exec = 0;
    u32 l = strlen(name);
    if (l > 5 && strcmp(name + l - 5, ".kapp") == 0) is_exec = 1;
    int r = kvfs_write(name, data, size, is_exec);
    return (r == 0) ? (int)size : -1;
}

static int kfs_remove(void *fs, const char *path) {
    (void)fs;
    return kvfs_delete(strip(path)) == 0 ? 0 : -1;
}

static void kfs_space(void *fs, u32 *total_kb, u32 *free_kb) {
    (void)fs;
    u32 total_mb = 0, used_kb = 0, files = 0;
    kvfs_stats(&total_mb, &used_kb, &files);
    if (total_kb) *total_kb = total_mb * 1024;
    if (free_kb)  *free_kb  = total_mb * 1024 - used_kb;
}

static const vfs_ops_t kvfs_ops = {
    kfs_readdir, kfs_read, kfs_size, kfs_stat,
    kfs_write, kfs_remove, NULL,
    kfs_space
};

void kvfs_vfs_register(void) {
    if (!kvfs_mounted()) return;
    vfs_mount("kv", FS_KVFS, &kvfs_ops, NULL, ata_boot_drive(), 2048, 0,
              "KvantOS", 1);
}
