/* ============================================================
 *  KvantOS - VFS: routing paths to the right filesystem driver
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"

static vfs_volume_t vols[VFS_MAX_VOLUMES];
static vfs_volume_t proc_volume; /* persistent system mount, not a disk */
static u32 generation;
u32 vfs_generation(void) { return generation; }

void vfs_namespace_changed(void) {if(!++generation)++generation;}

void vfs_init(void) {
    vfs_namespace_changed();
    memset(vols, 0, sizeof(vols));
    fat_reset_mounts();
    ntfs_reset_mounts();
}

const char *vfs_kind_name(fs_kind_t k) {
    switch (k) {
        case FS_RAMFS: return "ramfs";
        case FS_KVFS:  return "KvFS";
        case FS_FAT16: return "FAT16";
        case FS_FAT32: return "FAT32";
        case FS_NTFS:  return "NTFS";
        case FS_PROC:  return "procfs";
        case FS_UNKNOWN: return T("unknown", "неизвестно");
        default:       return "-";
    }
}

int vfs_mount(const char *name, fs_kind_t kind, const vfs_ops_t *ops,
              void *fs, int disk, u32 lba_start, u32 lba_count,
              const char *label, int writable) {
    if (!name) return -1;
    if (kind == FS_PROC) {
        if (!ops || strcmp(name, "proc") || proc_volume.used) return -1;
        memset(&proc_volume, 0, sizeof(proc_volume));
        strcpy(proc_volume.name, "proc");
        strcpy(proc_volume.label, "System information");
        proc_volume.kind = FS_PROC; proc_volume.ops = ops;
        proc_volume.fs = fs; proc_volume.disk = -1;
        proc_volume.used = 1; /* writable is ALWAYS zero */
        vfs_namespace_changed();
        return VFS_MAX_VOLUMES;
    }
    if (!strcmp(name, "proc")) return -1; /* cannot shadow the reserved name */
    for (int i = 0; i < VFS_MAX_VOLUMES; i++) {
        if (vols[i].used) continue;
        memset(&vols[i], 0, sizeof(vols[i]));
        strncpy(vols[i].name, name, sizeof(vols[i].name));
        if (label) strncpy(vols[i].label, label, sizeof(vols[i].label));
        vols[i].kind      = kind;
        vols[i].ops       = ops;
        vols[i].fs        = fs;
        vols[i].disk      = disk;
        vols[i].lba_start = lba_start;
        vols[i].lba_count = lba_count;
        vols[i].writable  = (u8)(writable && !blk_is_usb(disk));
        vols[i].used      = 1;
        return i;
    }
    return -1;
}

int vfs_volume_count(void) {
    int n = 0;
    for (int i = 0; i < VFS_MAX_VOLUMES; i++) if (vols[i].used) n++;
    return n + (proc_volume.used ? 1 : 0);
}

vfs_volume_t *vfs_volume(int i) {
    int n = 0;
    for (int k = 0; k < VFS_MAX_VOLUMES; k++) {
        if (!vols[k].used) continue;
        if (n == i) return &vols[k];
        n++;
    }
    if (proc_volume.used && i == n) return &proc_volume;
    return NULL;
}

vfs_volume_t *vfs_find(const char *name) {
    if (!name) return NULL;
    if (proc_volume.used && !strcmp(name, "proc")) return &proc_volume;
    for (int i = 0; i < VFS_MAX_VOLUMES; i++)
        if (vols[i].used && strcmp(vols[i].name, name) == 0) return &vols[i];
    return NULL;
}

/* Split "/mnt/hda1/dir/file" into the volume and the rest ("/dir/file").
   A bare "/mnt/hda1" yields "/". Returns NULL when the volume is not
   mounted or the path is malformed. */
static vfs_volume_t *resolve(const char *path, const char **rest) {
    if (!path || path[0] != '/') return NULL;

    const char *p = path;
    while (*p == '/') p++;

    /* the optional "mnt/" prefix */
    if (strncmp(p, "mnt", 3) == 0 && (p[3] == '/' || p[3] == 0)) {
        p += 3;
        while (*p == '/') p++;
    }

    char vol[16];
    u32 n = 0;
    while (*p && *p != '/' && n < sizeof(vol) - 1) vol[n++] = *p++;
    vol[n] = 0;
    if(*p && *p!='/')return NULL; /* never alias a truncated volume component */
    if (!n) return NULL;

    vfs_volume_t *v = vfs_find(vol);
    if (!v) return NULL;

    *rest = (*p == 0) ? "/" : p;
    return v;
}

int vfs_readdir(const char *path, int index, vfs_dirent_t *out) {
    if(!out || index<0)return -1;
    const char *rest;vfs_volume_t *v=resolve(path,&rest);
    if(!v || !v->ops || !v->ops->readdir)return -1;
    u32 epoch=vfs_generation();int r=v->ops->readdir(v->fs,rest,index,out);
    return epoch && epoch==vfs_generation() && (r==0 || r==1)?r:-1;
}

/* Other drivers keep their existing single-entry interface. No persistent
 * cursor/cache: every batch resolves the current namespace afresh. */
int vfs_readdir_many(const char *path,int first,vfs_dirent_t *out,int count) {
    if(!out || first<0 || count<1 || count>128 || first>0x7FFFFFFF-count) return -1;
    const char *rest;vfs_volume_t *v=resolve(path,&rest);
    if(!v || !v->ops) return -1;
    u32 epoch=vfs_generation();int n=0;
    if(v->ops->readdir_many)n=v->ops->readdir_many(v->fs,rest,first,out,count);
    else {
        if(!v->ops->readdir)return -1;
        while(n<count) {
            int r=v->ops->readdir(v->fs,rest,first+n,out+n);
            if(epoch!=vfs_generation() || (r!=0 && r!=1))return -1;
            if(!r)break;
            n++;
        }
    }
    return epoch && epoch==vfs_generation() && n>=0 && n<=count?n:-1;
}

int vfs_read(const char *path, u32 off, void *buf, u32 max) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v || !v->ops || !v->ops->read) return -1;
    return v->ops->read(v->fs, rest, off, buf, max);
}

u32 vfs_size(const char *path) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v || !v->ops || !v->ops->size) return 0;
    return v->ops->size(v->fs, rest);
}

int vfs_stat(const char *path) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v || !v->ops || !v->ops->stat) return 0;
    return v->ops->stat(v->fs, rest);
}

int vfs_write(const char *path, const void *data, u32 size) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v) return -1;
    if (!v->writable || !v->ops || !v->ops->write) return -2;   /* read-only */
    return v->ops->write(v->fs, rest, data, size);
}

int vfs_remove(const char *path) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v) return -1;
    if (!v->writable || !v->ops || !v->ops->remove) return -2;
    return v->ops->remove(v->fs, rest);
}

int vfs_can_rename(const char *path) {
    const char *rest;vfs_volume_t *v=resolve(path,&rest);
    return v && v->writable && v->ops && v->ops->rename;
}

int vfs_rename(const char *from,const char *to) {
    const char *paths[2]={from,to};
    for(int i=0;i<2;i++) {
        if(!paths[i])return -4;
        u32 n=0;while(n<VFS_MAX_PATH && paths[i][n])n++;
        if(!n || n==VFS_MAX_PATH)return -4;
    }
    const char *old_name,*new_name;
    vfs_volume_t *source=resolve(from,&old_name),*target=resolve(to,&new_name);
    if(!source || !target)return -1;
    if(source!=target)return -7;
    if(!source->writable || !source->ops || !source->ops->rename)return -2;
    return source->ops->rename(source->fs,old_name,new_name);
}

int vfs_mkdir(const char *path) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v) return -1;
    if (!v->writable || !v->ops || !v->ops->mkdir) return -2;
    return v->ops->mkdir(v->fs, rest);
}

void vfs_space(const char *path, u32 *total_kb, u32 *free_kb) {
    if (total_kb) *total_kb = 0;
    if (free_kb)  *free_kb  = 0;
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v || !v->ops || !v->ops->space) return;
    v->ops->space(v->fs, total_kb, free_kb);
}

int vfs_writable(const char *path) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v) return 0;
    return v->writable && v->ops && v->ops->write;
}

/* ---------- path helpers ---------- */

void vfs_join(char *dst, u32 dstsz, const char *dir, const char *name) {
    u32 n = 0;
    while (dir[n] && n < dstsz - 1) { dst[n] = dir[n]; n++; }
    if (n && dst[n - 1] != '/' && n < dstsz - 1) dst[n++] = '/';
    u32 i = 0;
    while (name[i] && n < dstsz - 1) dst[n++] = name[i++];
    dst[n] = 0;
}

void vfs_parent(char *dst, u32 dstsz, const char *path) {
    u32 len = 0;
    while (path[len] && len < dstsz - 1) { dst[len] = path[len]; len++; }
    dst[len] = 0;
    if (len == 0) return;
    if (dst[len - 1] == '/') { dst[len - 1] = 0; len--; }
    while (len > 0 && dst[len - 1] != '/') { dst[len - 1] = 0; len--; }
    if (len > 1 && dst[len - 1] == '/') dst[len - 1] = 0;
    if (dst[0] == 0) { dst[0] = '/'; dst[1] = 0; }
}

int vfs_is_root(const char *path) {
    const char *rest;
    vfs_volume_t *v = resolve(path, &rest);
    if (!v) return 1;
    return rest[0] == '/' && rest[1] == 0;
}
