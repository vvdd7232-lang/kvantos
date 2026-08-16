/* ============================================================
 *  KvantOS - FAT16/FAT32 driver (read and write)
 *
 *  Supports:
 *    - FAT16 and FAT32, sector sizes 512..4096
 *    - long file names (VFAT), including reading and creating them
 *    - subdirectories to any depth
 *    - creating, overwriting and deleting files, creating directories
 *    - the volume label from the boot sector and the root directory
 *
 *  Deliberately NOT supported: exFAT (a different format that merely
 *  shares the partition type), fragmented writes into the middle of a
 *  file (a write always replaces the whole file).
 *
 *  Long names are stored as UTF-16 by the format; the system speaks
 *  UTF-8, so the two are converted on the fly. Characters outside the
 *  Basic Multilingual Plane are not representable in a single UTF-16
 *  unit and are replaced with '_'.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"

#define FAT_MAX_VOLS      4
#define SECTOR_MAX        4096
#define FAT_ATTR_RO       0x01
#define FAT_ATTR_HIDDEN   0x02
#define FAT_ATTR_SYSTEM   0x04
#define FAT_ATTR_LABEL    0x08
#define FAT_ATTR_DIR      0x10
#define FAT_ATTR_ARCHIVE  0x20
#define FAT_ATTR_LFN      0x0F

#define FAT_EOC32         0x0FFFFFF8u
#define FAT_EOC16         0xFFF8u
#define FAT_FREE          0x00000000u

typedef struct {
    int  disk;
    u32  lba;                /* first sector of the volume       */
    u32  total_sectors;
    u32  bytes_per_sector;
    u32  sectors_per_cluster;
    u32  reserved_sectors;
    u32  num_fats;
    u32  fat_size;           /* sectors per FAT                  */
    u32  root_entries;       /* FAT16 only                       */
    u32  root_cluster;       /* FAT32 only                       */
    u32  first_data_sector;
    u32  total_clusters;
    u32  fat_start;
    u32  fsinfo_sector;      /* FAT32 only, 0 when absent */
    int  is_fat32;
    char label[VFS_LABEL_MAX];
    u8   used;
} fat_vol_t;

static fat_vol_t fat_vols[FAT_MAX_VOLS];

/* Sector buffers live in BSS rather than on the stack. A 4 KiB array in
   every frame added up to about 14 KiB along the deepest call chain
   (fat_op_write -> write_entry -> walk_dir), and a scheduler task only
   gets an 8 KiB stack - the system hung the moment a directory was
   listed from the GUI. Each function owns its own buffer, and no two of
   them are ever active at the same time: the call graph below has no
   cycles, and the driver is only entered from one task at a time. */
static u8 buf_get[SECTOR_MAX];        /* fat_get                */
static u8 buf_set[SECTOR_MAX];        /* fat_set                */
static u8 buf_fsinfo[SECTOR_MAX];     /* fsinfo_adjust          */
static u8 buf_walk[SECTOR_MAX];       /* walk_dir               */
static u8 buf_slots[SECTOR_MAX];      /* alloc_dir_slots        */
static u8 buf_entry[SECTOR_MAX];      /* write_entry            */
static u8 buf_erase[SECTOR_MAX];      /* erase_entry            */
static u8 buf_io[SECTOR_MAX];         /* fat_op_write / mkdir   */
static u8 buf_read[SECTOR_MAX];       /* fat_op_read            */

/* One on-disk 8.3 directory entry. */
typedef struct {
    u8  name[11];
    u8  attr;
    u8  nt;
    u8  ctime_tenth;
    u16 ctime;
    u16 cdate;
    u16 adate;
    u16 cluster_hi;
    u16 mtime;
    u16 mdate;
    u16 cluster_lo;
    u32 size;
} __attribute__((packed)) fat_dirent_t;

/* A long-name fragment: 13 UTF-16 units spread over three fields. */
typedef struct {
    u8  order;
    u16 name1[5];
    u8  attr;
    u8  type;
    u8  checksum;
    u16 name2[6];
    u16 cluster;
    u16 name3[2];
} __attribute__((packed)) fat_lfn_t;

/* ---------- sector helpers ---------- */

static int fat_read_sectors(fat_vol_t *v, u32 sec, u32 count, void *buf) {
    /* ata_read takes a sector count in one byte. */
    u8 *p = (u8 *)buf;
    while (count) {
        u8 chunk = (u8)(count > 64 ? 64 : count);
        if (!disk_read(v->disk, v->lba + sec, chunk, p)) return 0;
        p     += (u32)chunk * v->bytes_per_sector;
        sec   += chunk;
        count -= chunk;
    }
    return 1;
}

static int fat_write_sectors(fat_vol_t *v, u32 sec, u32 count, const void *buf) {
    const u8 *p = (const u8 *)buf;
    while (count) {
        u8 chunk = (u8)(count > 64 ? 64 : count);
        if (!disk_write(v->disk, v->lba + sec, chunk, p)) return 0;
        p     += (u32)chunk * v->bytes_per_sector;
        sec   += chunk;
        count -= chunk;
    }
    return 1;
}

static u32 cluster_to_sector(fat_vol_t *v, u32 clu) {
    return v->first_data_sector + (clu - 2) * v->sectors_per_cluster;
}

/* ---------- the FAT itself ---------- */

static u32 fat_get(fat_vol_t *v, u32 clu) {
    u8 *sec = buf_get;
    u32 off = v->is_fat32 ? clu * 4 : clu * 2;
    u32 s   = v->fat_start + off / v->bytes_per_sector;
    u32 o   = off % v->bytes_per_sector;

    if (!fat_read_sectors(v, s, 1, sec)) return v->is_fat32 ? FAT_EOC32 : FAT_EOC16;

    if (v->is_fat32) {
        u32 val;
        memcpy(&val, sec + o, 4);
        return val & 0x0FFFFFFFu;
    }
    u16 val;
    memcpy(&val, sec + o, 2);
    return val;
}

static int fat_set(fat_vol_t *v, u32 clu, u32 val) {
    u8 *sec = buf_set;
    u32 off = v->is_fat32 ? clu * 4 : clu * 2;
    u32 s   = v->fat_start + off / v->bytes_per_sector;
    u32 o   = off % v->bytes_per_sector;

    if (!fat_read_sectors(v, s, 1, sec)) return 0;

    if (v->is_fat32) {
        u32 old;
        memcpy(&old, sec + o, 4);
        u32 nv = (old & 0xF0000000u) | (val & 0x0FFFFFFFu);
        memcpy(sec + o, &nv, 4);
    } else {
        u16 nv = (u16)val;
        memcpy(sec + o, &nv, 2);
    }

    /* Every copy of the FAT must stay identical. */
    for (u32 f = 0; f < v->num_fats; f++)
        if (!fat_write_sectors(v, s + f * v->fat_size, 1, sec)) return 0;
    return 1;
}

static int fat_is_eoc(fat_vol_t *v, u32 clu) {
    return v->is_fat32 ? (clu >= FAT_EOC32) : (clu >= FAT_EOC16);
}

/* FAT32 keeps a cached free-cluster count in the FSInfo sector. It is
   only a hint, but Windows and fsck report a "wrong free count" when it
   drifts, so every allocation and release adjusts it. */
static void fsinfo_adjust(fat_vol_t *v, i32 delta) {
    if (!v->is_fat32 || !v->fsinfo_sector) return;

    u8 *sec = buf_fsinfo;
    if (!fat_read_sectors(v, v->fsinfo_sector, 1, sec)) return;

    u32 sig1, sig2;
    memcpy(&sig1, sec + 0, 4);
    memcpy(&sig2, sec + 484, 4);
    if (sig1 != 0x41615252u || sig2 != 0x61417272u) return;

    u32 freec;
    memcpy(&freec, sec + 488, 4);
    if (freec == 0xFFFFFFFFu) return;          /* unknown: leave it so */

    if (delta < 0 && freec < (u32)(-delta)) freec = 0;
    else                                    freec = (u32)((i32)freec + delta);

    memcpy(sec + 488, &freec, 4);
    fat_write_sectors(v, v->fsinfo_sector, 1, sec);
}

static u32 fat_alloc_cluster(fat_vol_t *v) {
    for (u32 c = 2; c < v->total_clusters + 2; c++) {
        if (fat_get(v, c) == FAT_FREE) {
            if (!fat_set(v, c, v->is_fat32 ? 0x0FFFFFFFu : 0xFFFFu)) return 0;
            fsinfo_adjust(v, -1);
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(fat_vol_t *v, u32 clu) {
    int guard = 0;
    while (clu >= 2 && !fat_is_eoc(v, clu) && guard++ < 1000000) {
        u32 next = fat_get(v, clu);
        fat_set(v, clu, FAT_FREE);
        fsinfo_adjust(v, +1);
        clu = next;
    }
}

/* ---------- name conversion ---------- */

/* Encode one code point as UTF-8. Returns the number of bytes. */
static u32 utf8_put(char *d, u32 cp) {
    if (cp < 0x80)   { d[0] = (char)cp; return 1; }
    if (cp < 0x800)  { d[0] = (char)(0xC0 | (cp >> 6));
                       d[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    d[0] = (char)(0xE0 | (cp >> 12));
    d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    d[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* Decode one UTF-8 character; advances *s. */
static u32 utf8_get(const char **s) {
    const u8 *p = (const u8 *)*s;
    u32 c = *p++;
    if (c < 0x80) { *s = (const char *)p; return c; }
    if ((c & 0xE0) == 0xC0) { u32 r = ((c & 0x1F) << 6) | (*p++ & 0x3F);
                              *s = (const char *)p; return r; }
    if ((c & 0xF0) == 0xE0) { u32 r = (c & 0x0F) << 12;
                              r |= (u32)(*p++ & 0x3F) << 6;
                              r |= (*p++ & 0x3F);
                              *s = (const char *)p; return r; }
    /* four-byte sequences do not fit a single UTF-16 unit */
    p += 3; *s = (const char *)p; return '_';
}

/* "README  TXT" -> "README.TXT" */
static void name83_to_str(const u8 *raw, char *out) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (raw[i] == ' ') break;
        out[n++] = (char)raw[i];
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11; i++) {
            if (raw[i] == ' ') break;
            out[n++] = (char)raw[i];
        }
    }
    out[n] = 0;
}

static char upcase(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Build an 8.3 name. Returns 1 when the name fits without loss. */
static int str_to_name83(const char *name, u8 *out) {
    for (int i = 0; i < 11; i++) out[i] = ' ';

    const char *dot = NULL;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;

    int lossless = 1;
    int n = 0;
    for (const char *p = name; *p && (!dot || p < dot); p++) {
        if ((u8)*p >= 0x80 || *p == ' ') { lossless = 0; continue; }
        if (n < 8) out[n++] = (u8)upcase(*p);
        else lossless = 0;
    }
    if (!n) { out[0] = '_'; lossless = 0; }

    if (dot) {
        int e = 0;
        for (const char *p = dot + 1; *p; p++) {
            if ((u8)*p >= 0x80) { lossless = 0; continue; }
            if (e < 3) out[8 + e++] = (u8)upcase(*p);
            else lossless = 0;
        }
    }
    /* a lower-case original still needs a long name to be preserved */
    for (const char *p = name; *p; p++)
        if (*p >= 'a' && *p <= 'z') { lossless = 0; break; }
    return lossless;
}

static u8 lfn_checksum(const u8 *name83) {
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name83[i]);
    return sum;
}

/* ---------- directory traversal ---------- */

/* Iterate the entries of a directory cluster chain (or the fixed FAT16
   root). `cb` is called for every real entry; returning 0 stops the
   walk. Long names are assembled here so callers never see them. */
typedef int (*dir_cb_t)(void *ctx, const char *name, fat_dirent_t *de,
                        u32 dir_sector, u32 entry_off);

static int walk_dir(fat_vol_t *v, u32 start_cluster, dir_cb_t cb, void *ctx) {
    u8 *sec = buf_walk;
    char lfn[VFS_MAX_NAME];
    int  have_lfn = 0;
    u16  lfn_units[260];
    int  lfn_max = 0;

    u32 clu = start_cluster;
    int fat16_root = (!v->is_fat32 && start_cluster == 0);

    u32 sector = 0, sectors_left = 0;
    if (fat16_root) {
        sector = v->fat_start + v->num_fats * v->fat_size;
        sectors_left = (v->root_entries * 32 + v->bytes_per_sector - 1) / v->bytes_per_sector;
    }

    int guard = 0;
    while (guard++ < 100000) {
        u32 run;
        if (fat16_root) {
            if (!sectors_left) break;
            run = 1;
        } else {
            if (clu < 2 || fat_is_eoc(v, clu)) break;
            sector = cluster_to_sector(v, clu);
            run = v->sectors_per_cluster;
        }

        for (u32 s = 0; s < run; s++) {
            if (!fat_read_sectors(v, sector + s, 1, sec)) return 0;

            for (u32 o = 0; o + 32 <= v->bytes_per_sector; o += 32) {
                fat_dirent_t *de = (fat_dirent_t *)(sec + o);

                if (de->name[0] == 0x00) return 1;          /* end of directory */
                if (de->name[0] == 0xE5) { have_lfn = 0; continue; }  /* deleted */

                if ((de->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                    fat_lfn_t *l = (fat_lfn_t *)de;
                    int idx = (l->order & 0x3F) - 1;
                    if (idx >= 0 && idx < 20) {
                        u16 *dst = lfn_units + idx * 13;
                        memcpy(dst,      l->name1, 10);
                        memcpy(dst + 5,  l->name2, 12);
                        memcpy(dst + 11, l->name3, 4);
                        if (idx + 1 > lfn_max) lfn_max = idx + 1;
                        have_lfn = 1;
                    }
                    continue;
                }

                if (de->attr & FAT_ATTR_LABEL) { have_lfn = 0; continue; }

                char name[VFS_MAX_NAME];
                if (have_lfn && lfn_max) {
                    u32 n = 0;
                    for (int i = 0; i < lfn_max * 13 && n < sizeof(name) - 4; i++) {
                        u16 u = lfn_units[i];
                        if (u == 0x0000 || u == 0xFFFF) break;
                        n += utf8_put(name + n, u);
                    }
                    name[n] = 0;
                } else {
                    name83_to_str(de->name, name);
                }
                have_lfn = 0; lfn_max = 0;
                (void)lfn;

                if (!cb(ctx, name, de, sector + s, o)) return 1;
            }
        }

        if (fat16_root) { sector++; sectors_left--; }
        else            { clu = fat_get(v, clu); }
    }
    return 1;
}

/* ---------- path lookup ---------- */

typedef struct {
    const char  *want;
    int          found;
    fat_dirent_t de;
    u32          sector;
    u32          off;
} find_ctx_t;

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = upcase(*a), cb = upcase(*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int find_cb(void *ctx, const char *name, fat_dirent_t *de,
                   u32 sector, u32 off) {
    find_ctx_t *f = (find_ctx_t *)ctx;
    if (streq_ci(name, f->want)) {
        f->found  = 1;
        f->de     = *de;
        f->sector = sector;
        f->off    = off;
        return 0;
    }
    return 1;
}

static u32 entry_cluster(fat_vol_t *v, fat_dirent_t *de) {
    u32 c = de->cluster_lo;
    if (v->is_fat32) c |= (u32)de->cluster_hi << 16;
    return c;
}

/* Resolve "/dir/sub/file". On success fills *out (when not NULL) and
   returns the starting cluster; 0 means the volume root. Returns -1
   when some component is missing. */
static int resolve_path(fat_vol_t *v, const char *path, fat_dirent_t *out,
                        u32 *out_sector, u32 *out_off, u32 *out_cluster) {
    u32 clu = v->is_fat32 ? v->root_cluster : 0;
    if (out) memset(out, 0, sizeof(*out));
    if (out_cluster) *out_cluster = clu;

    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return 0;                    /* the root itself */

    while (*p) {
        char comp[VFS_MAX_NAME];
        u32 n = 0;
        while (*p && *p != '/' && n < sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;

        find_ctx_t f; f.want = comp; f.found = 0; f.sector = 0; f.off = 0; memset(&f.de, 0, sizeof(f.de));
        walk_dir(v, clu, find_cb, &f);
        if (!f.found) return -1;

        clu = entry_cluster(v, &f.de);
        if (out)        *out = f.de;
        if (out_sector) *out_sector = f.sector;
        if (out_off)    *out_off = f.off;
        if (out_cluster) *out_cluster = clu;

        if (*p && !(f.de.attr & FAT_ATTR_DIR)) return -1;  /* not a directory */
    }
    return 1;
}

/* ---------- VFS operations ---------- */

typedef struct {
    int   index;
    int   want;
    vfs_dirent_t *out;
    int   done;
    fat_vol_t *v;
} list_ctx_t;

static int list_cb(void *ctx, const char *name, fat_dirent_t *de,
                   u32 sector, u32 off) {
    list_ctx_t *l = (list_ctx_t *)ctx;
    (void)sector; (void)off;

    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        return 1;                                   /* skip . and .. */

    if (l->index == l->want) {
        strncpy(l->out->name, name, VFS_MAX_NAME);
        l->out->is_dir      = (de->attr & FAT_ATTR_DIR) ? 1 : 0;
        l->out->is_readonly = (de->attr & FAT_ATTR_RO) ? 1 : 0;
        l->out->is_hidden   = (de->attr & FAT_ATTR_HIDDEN) ? 1 : 0;
        l->out->is_system   = (de->attr & FAT_ATTR_SYSTEM) ? 1 : 0;
        l->out->size        = l->out->is_dir ? 0 : de->size;
        l->out->mtime_year  = (u16)(1980 + ((de->mdate >> 9) & 0x7F));
        l->out->mtime_month = (u8)((de->mdate >> 5) & 0x0F);
        l->out->mtime_day   = (u8)(de->mdate & 0x1F);
        l->out->mtime_hour  = (u8)((de->mtime >> 11) & 0x1F);
        l->out->mtime_min   = (u8)((de->mtime >> 5) & 0x3F);
        l->done = 1;
        return 0;
    }
    l->index++;
    return 1;
}

static int fat_op_readdir(void *fs, const char *path, int index, vfs_dirent_t *out) {
    fat_vol_t *v = (fat_vol_t *)fs;
    fat_dirent_t de;
    u32 clu;

    if (resolve_path(v, path, &de, NULL, NULL, &clu) < 0) return 0;

    memset(out, 0, sizeof(*out));
    list_ctx_t l = { 0, index, out, 0, v };
    walk_dir(v, clu, list_cb, &l);
    return l.done;
}

static int fat_op_stat(void *fs, const char *path) {
    fat_vol_t *v = (fat_vol_t *)fs;
    fat_dirent_t de;
    int r = resolve_path(v, path, &de, NULL, NULL, NULL);
    if (r < 0) return 0;
    if (r == 0) return 2;                       /* the root is a directory */
    return (de.attr & FAT_ATTR_DIR) ? 2 : 1;
}

static u32 fat_op_size(void *fs, const char *path) {
    fat_vol_t *v = (fat_vol_t *)fs;
    fat_dirent_t de;
    if (resolve_path(v, path, &de, NULL, NULL, NULL) != 1) return 0;
    return (de.attr & FAT_ATTR_DIR) ? 0 : de.size;
}

static int fat_op_read(void *fs, const char *path, u32 off, void *buf, u32 max) {
    fat_vol_t *v = (fat_vol_t *)fs;
    fat_dirent_t de;
    u32 clu;

    if (resolve_path(v, path, &de, NULL, NULL, &clu) != 1) return -1;
    if (de.attr & FAT_ATTR_DIR) return -1;
    if (off >= de.size) return 0;

    u32 want = de.size - off;
    if (want > max) want = max;

    u32 cluster_bytes = v->sectors_per_cluster * v->bytes_per_sector;

    /* skip whole clusters up to the offset */
    u32 skip = off / cluster_bytes;
    for (u32 i = 0; i < skip; i++) {
        if (clu < 2 || fat_is_eoc(v, clu)) return 0;
        clu = fat_get(v, clu);
    }
    u32 inner = off % cluster_bytes;

    u8 *dst = (u8 *)buf;
    u32 done = 0;
    u8 *sec = buf_read;

    while (done < want && clu >= 2 && !fat_is_eoc(v, clu)) {
        u32 base = cluster_to_sector(v, clu);
        for (u32 s = 0; s < v->sectors_per_cluster && done < want; s++) {
            u32 sec_off = s * v->bytes_per_sector;
            if (sec_off + v->bytes_per_sector <= inner) continue;

            if (!fat_read_sectors(v, base + s, 1, sec)) return (int)done;

            u32 from = (inner > sec_off) ? (inner - sec_off) : 0;
            u32 n    = v->bytes_per_sector - from;
            if (n > want - done) n = want - done;
            memcpy(dst + done, sec + from, n);
            done += n;
        }
        inner = 0;
        clu = fat_get(v, clu);
    }
    return (int)done;
}

/* Clusters to kibibytes without 64-bit division: a cluster can be
   smaller than 1 KiB, so the count is converted through sectors. */
static u32 clusters_to_kb(fat_vol_t *v, u32 clusters) {
    u32 sectors = clusters * v->sectors_per_cluster;
    return (sectors / 2) * (v->bytes_per_sector / 512);
}

static void fat_op_space(void *fs, u32 *total_kb, u32 *free_kb) {
    fat_vol_t *v = (fat_vol_t *)fs;
    if (total_kb) *total_kb = clusters_to_kb(v, v->total_clusters);

    if (free_kb) {
        /* Counting every FAT entry on a large volume would read many
           megabytes, so the scan samples every `step`-th cluster and
           multiplies the result back. */
        /* Every fat_get reads a sector, so a full scan of a 260 MiB
           FAT32 volume would be half a million reads. Sampling 4096
           clusters is accurate to a fraction of a percent and finishes
           instantly. */
        u32 step = 1;
        if (v->total_clusters > 4096) step = v->total_clusters / 4096;

        u32 checked = 0, freec = 0;
        for (u32 c = 2; c < v->total_clusters + 2; c += step) {
            if (fat_get(v, c) == FAT_FREE) freec++;
            if (++checked > 4096) break;
        }
        *free_kb = clusters_to_kb(v, freec * step);
    }
}

/* ---------- writing ---------- */

/* Find a run of `need` consecutive free 32-byte slots in a directory,
   extending the chain when necessary. Returns the sector and offset of
   the first slot. */
typedef struct {
    u32 need;
    u32 run;
    u32 first_sector, first_off;
    int found;
} slot_ctx_t;

static int alloc_dir_slots(fat_vol_t *v, u32 dir_cluster, u32 need,
                           u32 *out_sector, u32 *out_off) {
    u8 *sec = buf_slots;
    u32 clu = dir_cluster;
    int fat16_root = (!v->is_fat32 && dir_cluster == 0);

    u32 sector = 0, sectors_left = 0;
    if (fat16_root) {
        sector = v->fat_start + v->num_fats * v->fat_size;
        sectors_left = (v->root_entries * 32 + v->bytes_per_sector - 1) / v->bytes_per_sector;
    }

    u32 run = 0, first_sector = 0, first_off = 0;
    u32 last_cluster = clu;

    int guard = 0;
    while (guard++ < 100000) {
        u32 span;
        if (fat16_root) {
            if (!sectors_left) break;
            span = 1;
        } else {
            if (clu < 2 || fat_is_eoc(v, clu)) break;
            last_cluster = clu;
            sector = cluster_to_sector(v, clu);
            span = v->sectors_per_cluster;
        }

        for (u32 s = 0; s < span; s++) {
            if (!fat_read_sectors(v, sector + s, 1, sec)) return 0;
            for (u32 o = 0; o + 32 <= v->bytes_per_sector; o += 32) {
                u8 first = sec[o];
                if (first == 0x00 || first == 0xE5) {
                    if (!run) { first_sector = sector + s; first_off = o; }
                    if (++run == need) {
                        *out_sector = first_sector;
                        *out_off    = first_off;
                        return 1;
                    }
                } else {
                    run = 0;
                }
            }
        }

        if (fat16_root) { sector++; sectors_left--; }
        else            { clu = fat_get(v, clu); }
    }

    /* The FAT16 root directory has a fixed size and cannot grow. */
    if (fat16_root) return 0;

    /* Extend the directory by one cluster and use it. */
    u32 nc = fat_alloc_cluster(v);
    if (!nc) return 0;
    if (!fat_set(v, last_cluster, nc)) return 0;

    static u8 zero[SECTOR_MAX];
    memset(zero, 0, v->bytes_per_sector);
    u32 base = cluster_to_sector(v, nc);
    for (u32 s = 0; s < v->sectors_per_cluster; s++)
        if (!fat_write_sectors(v, base + s, 1, zero)) return 0;

    *out_sector = base;
    *out_off    = 0;
    return 1;
}

/* Write the 8.3 entry plus its long-name fragments. */
static int write_entry(fat_vol_t *v, u32 dir_cluster, const char *name,
                       u32 cluster, u32 size, u8 attr) {
    u8 n83[11];
    int lossless = str_to_name83(name, n83);

    /* how many UTF-16 units the long name needs */
    u16 units[260];
    u32 ulen = 0;
    {
        const char *p = name;
        while (*p && ulen < 255) units[ulen++] = (u16)utf8_get(&p);
    }
    units[ulen] = 0;

    u32 lfn_count = lossless ? 0 : (ulen + 12) / 13;
    if (lfn_count > 20) lfn_count = 20;

    /* An 8.3 collision would make two different files share a short
       name, so add a ~N tail until it is unique. */
    if (lfn_count) {
        for (int attempt = 1; attempt < 100; attempt++) {
            char probe[13];
            name83_to_str(n83, probe);
            find_ctx_t f; f.want = probe; f.found = 0; f.sector = 0; f.off = 0; memset(&f.de, 0, sizeof(f.de));
            walk_dir(v, dir_cluster, find_cb, &f);
            if (!f.found) break;
            char tail[8];
            ksnprintf(tail, sizeof(tail), "~%d", attempt);
            u32 tl = strlen(tail);
            for (u32 i = 0; i < tl; i++) n83[8 - tl + i] = (u8)tail[i];
        }
    }

    u32 need = lfn_count + 1;
    u32 sector, off;
    if (!alloc_dir_slots(v, dir_cluster, need, &sector, &off)) return 0;

    u8 sum = lfn_checksum(n83);
    u8 *sec = buf_entry;

    /* Fragments are stored in reverse order, last one first. */
    for (u32 i = 0; i < lfn_count; i++) {
        u32 idx = lfn_count - i;                 /* 1-based */
        u32 pos = off + i * 32;
        u32 s   = sector + pos / v->bytes_per_sector;
        u32 o   = pos % v->bytes_per_sector;

        if (!fat_read_sectors(v, s, 1, sec)) return 0;

        fat_lfn_t l;
        memset(&l, 0xFF, sizeof(l));
        l.order    = (u8)(idx | (i == 0 ? 0x40 : 0));
        l.attr     = FAT_ATTR_LFN;
        l.type     = 0;
        l.checksum = sum;
        l.cluster  = 0;

        u32 base = (idx - 1) * 13;
        u16 tmp[13];
        for (int k = 0; k < 13; k++) {
            u32 ci = base + (u32)k;
            tmp[k] = (ci < ulen) ? units[ci] : (ci == ulen ? 0x0000 : 0xFFFF);
        }
        memcpy(l.name1, tmp,      10);
        memcpy(l.name2, tmp + 5,  12);
        memcpy(l.name3, tmp + 11, 4);

        memcpy(sec + o, &l, 32);
        if (!fat_write_sectors(v, s, 1, sec)) return 0;
    }

    /* the real entry */
    u32 pos = off + lfn_count * 32;
    u32 s   = sector + pos / v->bytes_per_sector;
    u32 o   = pos % v->bytes_per_sector;

    if (!fat_read_sectors(v, s, 1, sec)) return 0;

    fat_dirent_t de;
    memset(&de, 0, sizeof(de));
    memcpy(de.name, n83, 11);
    de.attr       = attr;
    de.cluster_lo = (u16)(cluster & 0xFFFF);
    de.cluster_hi = (u16)(cluster >> 16);
    de.size       = size;

    rtc_time_t tm;
    rtc_read(&tm);
    u32 year = (tm.year >= 1980) ? (tm.year - 1980) : 0;
    de.mdate = (u16)((year << 9) | ((u32)tm.month << 5) | tm.day);
    de.mtime = (u16)(((u32)tm.hour << 11) | ((u32)tm.min << 5) | (tm.sec / 2));
    de.cdate = de.adate = de.mdate;
    de.ctime = de.mtime;

    memcpy(sec + o, &de, 32);
    return fat_write_sectors(v, s, 1, sec);
}

/* Mark an entry and its long-name fragments as deleted. */
static int erase_entry(fat_vol_t *v, u32 dir_cluster, const char *name) {
    /* Find the 8.3 entry first, then walk backwards over the LFN parts
       that immediately precede it. */
    find_ctx_t f; f.want = name; f.found = 0; f.sector = 0; f.off = 0; memset(&f.de, 0, sizeof(f.de));
    walk_dir(v, dir_cluster, find_cb, &f);
    if (!f.found) return 0;

    u8 *sec = buf_erase;
    if (!fat_read_sectors(v, f.sector, 1, sec)) return 0;
    sec[f.off] = 0xE5;
    if (!fat_write_sectors(v, f.sector, 1, sec)) return 0;

    /* preceding fragments inside the same sector */
    i32 o = (i32)f.off - 32;
    while (o >= 0) {
        if ((sec[o + 11] & FAT_ATTR_LFN) != FAT_ATTR_LFN) break;
        sec[o] = 0xE5;
        o -= 32;
    }
    return fat_write_sectors(v, f.sector, 1, sec);
}

/* Split "/dir/name" into the parent cluster and the final component. */
static int split_parent(fat_vol_t *v, const char *path, u32 *parent_cluster,
                        char *leaf, u32 leafsz) {
    char dir[VFS_MAX_PATH];
    u32 len = 0;
    while (path[len] && len < sizeof(dir) - 1) { dir[len] = path[len]; len++; }
    dir[len] = 0;

    /* strip a trailing slash */
    while (len > 1 && dir[len - 1] == '/') dir[--len] = 0;

    i32 cut = -1;
    for (i32 i = (i32)len - 1; i >= 0; i--) if (dir[i] == '/') { cut = i; break; }
    if (cut < 0) return 0;

    u32 n = 0;
    for (u32 i = (u32)cut + 1; dir[i] && n < leafsz - 1; i++) leaf[n++] = dir[i];
    leaf[n] = 0;
    if (!n) return 0;

    dir[cut] = 0;
    const char *parent = (cut == 0) ? "/" : dir;

    fat_dirent_t de;
    u32 clu;
    if (resolve_path(v, parent, &de, NULL, NULL, &clu) < 0) return 0;
    *parent_cluster = clu;
    return 1;
}

static int fat_op_write(void *fs, const char *path, const void *data, u32 size) {
    fat_vol_t *v = (fat_vol_t *)fs;

    u32 parent;
    char leaf[VFS_MAX_NAME];
    if (!split_parent(v, path, &parent, leaf, sizeof(leaf))) return -1;

    /* replacing an existing file: drop the old contents first */
    fat_dirent_t old;
    if (resolve_path(v, path, &old, NULL, NULL, NULL) == 1) {
        u32 oc = entry_cluster(v, &old);
        if (oc >= 2) fat_free_chain(v, oc);
        erase_entry(v, parent, leaf);
    }

    u32 cluster_bytes = v->sectors_per_cluster * v->bytes_per_sector;
    u32 first = 0, prev = 0;
    const u8 *src = (const u8 *)data;
    u32 done = 0;
    u8 *sec = buf_io;

    while (done < size) {
        u32 c = fat_alloc_cluster(v);
        if (!c) { if (first) fat_free_chain(v, first); return -3; }   /* disk full */
        if (!first) first = c;
        else if (!fat_set(v, prev, c)) return -1;
        prev = c;

        u32 base = cluster_to_sector(v, c);
        for (u32 s = 0; s < v->sectors_per_cluster && done < size; s++) {
            u32 n = size - done;
            if (n > v->bytes_per_sector) n = v->bytes_per_sector;
            memset(sec, 0, v->bytes_per_sector);
            memcpy(sec, src + done, n);
            if (!fat_write_sectors(v, base + s, 1, sec)) return -1;
            done += n;
        }
        (void)cluster_bytes;
    }

    if (!write_entry(v, parent, leaf, first, size, FAT_ATTR_ARCHIVE)) {
        if (first) fat_free_chain(v, first);
        return -1;
    }
    return (int)size;
}

static int fat_op_remove(void *fs, const char *path) {
    fat_vol_t *v = (fat_vol_t *)fs;

    fat_dirent_t de;
    if (resolve_path(v, path, &de, NULL, NULL, NULL) != 1) return -1;

    /* refuse to delete a directory that still has entries */
    if (de.attr & FAT_ATTR_DIR) {
        vfs_dirent_t probe;
        list_ctx_t l = { 0, 0, &probe, 0, v };
        walk_dir(v, entry_cluster(v, &de), list_cb, &l);
        if (l.done) return -4;                    /* not empty */
    }

    u32 parent;
    char leaf[VFS_MAX_NAME];
    if (!split_parent(v, path, &parent, leaf, sizeof(leaf))) return -1;

    u32 c = entry_cluster(v, &de);
    if (c >= 2) fat_free_chain(v, c);
    return erase_entry(v, parent, leaf) ? 0 : -1;
}

static int fat_op_mkdir(void *fs, const char *path) {
    fat_vol_t *v = (fat_vol_t *)fs;

    if (fat_op_stat(fs, path)) return -5;         /* already exists */

    u32 parent;
    char leaf[VFS_MAX_NAME];
    if (!split_parent(v, path, &parent, leaf, sizeof(leaf))) return -1;

    u32 c = fat_alloc_cluster(v);
    if (!c) return -3;

    /* a fresh directory holds exactly "." and ".." */
    u8 *sec = buf_io;
    memset(sec, 0, v->bytes_per_sector);

    fat_dirent_t dot;
    memset(&dot, 0, sizeof(dot));
    memcpy(dot.name, ".          ", 11);
    dot.attr       = FAT_ATTR_DIR;
    dot.cluster_lo = (u16)(c & 0xFFFF);
    dot.cluster_hi = (u16)(c >> 16);
    memcpy(sec, &dot, 32);

    fat_dirent_t dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    memcpy(dotdot.name, "..         ", 11);
    dotdot.attr = FAT_ATTR_DIR;
    u32 pc = (parent == v->root_cluster && v->is_fat32) ? 0 : parent;
    dotdot.cluster_lo = (u16)(pc & 0xFFFF);
    dotdot.cluster_hi = (u16)(pc >> 16);
    memcpy(sec + 32, &dotdot, 32);

    u32 base = cluster_to_sector(v, c);
    if (!fat_write_sectors(v, base, 1, sec)) return -1;

    memset(sec, 0, v->bytes_per_sector);
    for (u32 s = 1; s < v->sectors_per_cluster; s++)
        if (!fat_write_sectors(v, base + s, 1, sec)) return -1;

    if (!write_entry(v, parent, leaf, c, 0, FAT_ATTR_DIR)) {
        fat_free_chain(v, c);
        return -1;
    }
    return 0;
}

static const vfs_ops_t fat_ops = {
    fat_op_readdir, fat_op_read, fat_op_size, fat_op_stat,
    fat_op_write, fat_op_remove, fat_op_mkdir, fat_op_space
};

/* ---------- probing and mounting ---------- */

/* Parse the BPB. Returns 1 when this really looks like FAT. */
static int parse_bpb(const u8 *b, fat_vol_t *v, char *label, fs_kind_t *kind) {
    u16 bps;
    memcpy(&bps, b + 11, 2);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;

    u8 spc = b[13];
    if (!spc || (spc & (spc - 1))) return 0;          /* must be a power of two */

    u16 reserved;
    memcpy(&reserved, b + 14, 2);
    if (!reserved) return 0;

    u8 nfats = b[16];
    if (nfats < 1 || nfats > 4) return 0;

    u16 root_ent;
    memcpy(&root_ent, b + 17, 2);

    u16 tot16;
    memcpy(&tot16, b + 19, 2);
    u32 tot32;
    memcpy(&tot32, b + 32, 4);
    u32 total = tot16 ? tot16 : tot32;
    if (!total) return 0;

    u16 fatsz16;
    memcpy(&fatsz16, b + 22, 2);
    u32 fatsz32;
    memcpy(&fatsz32, b + 36, 4);
    u32 fatsz = fatsz16 ? fatsz16 : fatsz32;
    if (!fatsz) return 0;

    v->bytes_per_sector    = bps;
    v->sectors_per_cluster = spc;
    v->reserved_sectors    = reserved;
    v->num_fats            = nfats;
    v->root_entries        = root_ent;
    v->fat_size            = fatsz;
    v->total_sectors       = total;
    v->fat_start           = reserved;

    u32 root_sectors = ((u32)root_ent * 32 + bps - 1) / bps;
    v->first_data_sector = reserved + nfats * fatsz + root_sectors;
    if (v->first_data_sector >= total) return 0;

    u32 data_sectors = total - v->first_data_sector;
    v->total_clusters = data_sectors / spc;
    if (!v->total_clusters) return 0;

    v->is_fat32 = (v->total_clusters >= 65525);
    if (v->is_fat32) {
        memcpy(&v->root_cluster, b + 44, 4);
        if (v->root_cluster < 2) return 0;
        u16 fsi;
        memcpy(&fsi, b + 48, 2);
        v->fsinfo_sector = (fsi && fsi != 0xFFFF) ? fsi : 0;
        if (kind) *kind = FS_FAT32;
    } else {
        v->root_cluster = 0;
        if (kind) *kind = FS_FAT16;
    }

    /* The label lives at a different offset in the two variants. */
    if (label) {
        const u8 *src = v->is_fat32 ? b + 71 : b + 43;
        int n = 0;
        for (int i = 0; i < 11; i++) {
            char c = (char)src[i];
            if (c == ' ' && n == 0) continue;
            label[n++] = c;
        }
        while (n > 0 && label[n - 1] == ' ') n--;
        label[n] = 0;
    }
    return 1;
}

int fat_probe(int disk, u32 lba_start, u32 lba_count, char *label, fs_kind_t *kind) {
    static u8 b[512];
    static fat_vol_t tmp;
    if (!disk_read(disk, lba_start, 1, b)) return 0;

    /* exFAT shares the NTFS/FAT partition type but is a different
       format entirely - recognise and refuse it explicitly. */
    if (memcmp(b + 3, "EXFAT   ", 8) == 0) return 0;
    if (memcmp(b + 3, "NTFS    ", 8) == 0) return 0;

    memset(&tmp, 0, sizeof(tmp));
    tmp.disk = disk;
    tmp.lba  = lba_start;

    if (!parse_bpb(b, &tmp, label, kind)) return 0;
    if (tmp.total_sectors > lba_count && lba_count) return 0;
    return 1;
}

int fat_mount(const char *name, int disk, u32 lba_start, u32 lba_count) {
    int slot = -1;
    for (int i = 0; i < FAT_MAX_VOLS; i++) if (!fat_vols[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    fat_vol_t *v = &fat_vols[slot];
    memset(v, 0, sizeof(*v));
    v->disk = disk;
    v->lba  = lba_start;

    static u8 b[512];
    if (!disk_read(disk, lba_start, 1, b)) return -1;

    fs_kind_t kind = FS_FAT32;
    if (!parse_bpb(b, v, v->label, &kind)) return -1;
    (void)lba_count;

    /* A FAT32 volume label is usually only in the root directory. */
    if (!v->label[0]) strncpy(v->label, T("no label", "без метки"), sizeof(v->label));

    v->used = 1;
    return vfs_mount(name, kind, &fat_ops, v, disk, lba_start,
                     v->total_sectors, v->label, 1);
}
