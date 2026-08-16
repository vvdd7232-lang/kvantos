/* ============================================================
 *  KvantOS - NTFS driver (read-only)
 *
 *  NTFS is not a "table of files" like FAT: everything on the volume,
 *  including the metadata, is a file described by a record in the
 *  Master File Table. A record is a list of attributes; the contents
 *  live in the $DATA attribute, which is either stored inline
 *  ("resident", for tiny files) or scattered across the disk as a
 *  "runlist" of extents. Directories are B-trees keyed by file name,
 *  built from $INDEX_ROOT and $INDEX_ALLOCATION.
 *
 *  Implemented here:
 *    - boot sector, MFT location, records of any size
 *    - the update-sequence fixup every record and index block needs
 *    - resident and non-resident attributes, runlists with sparse runs
 *    - $ATTRIBUTE_LIST, so files whose attributes spill into extension
 *      records still work
 *    - directory B-trees: $INDEX_ROOT plus $INDEX_ALLOCATION blocks
 *    - long file names in UTF-16, the volume label from $Volume
 *
 *  NOT implemented, and reported honestly instead of guessed:
 *    - writing. NTFS journals every change through $LogFile; a driver
 *      that writes without maintaining the journal, the bitmaps and
 *      the B-tree balance would corrupt volumes that Windows then
 *      refuses to mount. Volumes are mounted read-only.
 *    - compressed and encrypted streams: the data is returned as an
 *      error rather than as garbage.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"

#define NTFS_MAX_VOLS     2
#define NTFS_MAX_RUNS     192
#define NTFS_REC_MAX      4096
#define NTFS_IDX_MAX      8192

/* attribute types */
#define AT_STANDARD       0x10
#define AT_ATTR_LIST      0x20
#define AT_FILE_NAME      0x30
#define AT_VOLUME_NAME    0x60
#define AT_DATA           0x80
#define AT_INDEX_ROOT     0x90
#define AT_INDEX_ALLOC    0xA0
#define AT_BITMAP         0xB0
#define AT_END            0xFFFFFFFFu

/* $DATA attribute flags */
#define ATTR_COMPRESSED   0x0001
#define ATTR_ENCRYPTED    0x4000
#define ATTR_SPARSE       0x8000

#define MFT_REC_ROOT      5      /* the root directory */
#define MFT_REC_VOLUME    3

typedef struct {
    u32 vcn;          /* first virtual cluster of the extent */
    u32 lcn;          /* logical cluster on disk, 0 = sparse  */
    u32 len;          /* length in clusters                   */
    u8  sparse;
} ntfs_run_t;

typedef struct {
    int  disk;
    u32  lba;
    u32  bytes_per_sector;
    u32  sectors_per_cluster;
    u32  cluster_size;
    u32  record_size;        /* size of one MFT record  */
    u32  index_size;         /* size of one index block */
    u32  mft_lcn;
    u32  total_sectors;
    ntfs_run_t mft_runs[NTFS_MAX_RUNS];
    int  mft_run_count;
    char label[VFS_LABEL_MAX];
    u8   used;
} ntfs_vol_t;

static ntfs_vol_t ntfs_vols[NTFS_MAX_VOLS];

/* One shared scratch record: the driver is single-threaded and these
   buffers are too large to put on the kernel stack. */
/* Like the FAT driver, every large buffer lives in BSS: an 8 KiB task
   stack cannot hold a 4 KiB MFT record plus a boot sector plus the
   frames above them. */
static u8 rec_buf[NTFS_REC_MAX];
static u8 idx_buf[NTFS_IDX_MAX];
static u8 sec_buf[4096];

/* ---------- low level ---------- */

static int ntfs_read_sectors(ntfs_vol_t *v, u32 sec, u32 count, void *buf) {
    u8 *p = (u8 *)buf;
    while (count) {
        u8 chunk = (u8)(count > 64 ? 64 : count);
        if (!disk_read(v->disk, v->lba + sec, chunk, p)) return 0;
        p     += (u32)chunk * 512;
        sec   += chunk;
        count -= chunk;
    }
    return 1;
}

static int read_clusters(ntfs_vol_t *v, u32 lcn, u32 count, void *buf) {
    return ntfs_read_sectors(v, lcn * v->sectors_per_cluster,
                             count * v->sectors_per_cluster, buf);
}

/* Every record and index block is protected by an update sequence: the
   last two bytes of each sector are replaced by a marker, and the real
   values are kept in an array at the start. Undo that here or the tail
   of every sector reads as nonsense. */
static int apply_fixup(ntfs_vol_t *v, u8 *buf, u32 size) {
    u16 usa_off, usa_count;
    memcpy(&usa_off,   buf + 0x04, 2);
    memcpy(&usa_count, buf + 0x06, 2);

    if (!usa_count || usa_off + usa_count * 2u > size) return 0;

    u16 marker;
    memcpy(&marker, buf + usa_off, 2);

    for (u32 i = 1; i < usa_count; i++) {
        u32 tail = i * v->bytes_per_sector - 2;
        if (tail + 2 > size) return 0;

        u16 have;
        memcpy(&have, buf + tail, 2);
        if (have != marker) return 0;            /* damaged block */

        memcpy(buf + tail, buf + usa_off + i * 2, 2);
    }
    return 1;
}

/* Translate a virtual cluster to a logical one using a runlist. */
static int run_lookup(const ntfs_run_t *runs, int count, u32 vcn,
                      u32 *lcn, u32 *left, int *sparse) {
    for (int i = 0; i < count; i++) {
        if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].len) {
            u32 delta = vcn - runs[i].vcn;
            *sparse = runs[i].sparse;
            *lcn    = runs[i].sparse ? 0 : runs[i].lcn + delta;
            *left   = runs[i].len - delta;
            return 1;
        }
    }
    return 0;
}

/* Decode the compressed runlist that follows a non-resident attribute. */
static int parse_runlist(const u8 *p, const u8 *end, u32 start_vcn,
                         ntfs_run_t *runs, int max) {
    int n = 0;
    i32 lcn = 0;
    u32 vcn = start_vcn;

    while (p < end && *p && n < max) {
        u8 header = *p++;
        u32 len_size = header & 0x0F;
        u32 off_size = (header >> 4) & 0x0F;
        if (!len_size || len_size > 8 || off_size > 8) break;
        if (p + len_size + off_size > end) break;

        /* run length: unsigned, little endian, variable width */
        u32 length = 0;
        for (u32 i = 0; i < len_size && i < 4; i++)
            length |= (u32)p[i] << (i * 8);
        p += len_size;

        int sparse = (off_size == 0);
        if (!sparse) {
            /* run offset: signed delta from the previous run */
            i32 delta = 0;
            for (u32 i = 0; i < off_size && i < 4; i++)
                delta |= (i32)((u32)p[i] << (i * 8));
            /* sign-extend from the top byte actually present */
            if (off_size <= 4 && (p[off_size - 1] & 0x80)) {
                u32 shift = off_size * 8;
                if (shift < 32) delta |= (i32)(0xFFFFFFFFu << shift);
            }
            p += off_size;
            lcn += delta;
        }

        if (!length) break;

        runs[n].vcn    = vcn;
        runs[n].len    = length;
        runs[n].sparse = (u8)(sparse ? 1 : 0);
        runs[n].lcn    = sparse ? 0 : (u32)lcn;
        n++;
        vcn += length;
    }
    return n;
}

/* ---------- MFT records ---------- */

/* Read MFT record number `num` into `dst`. */
static int read_mft_record(ntfs_vol_t *v, u32 num, u8 *dst) {
    u32 recs_per_cluster = v->cluster_size / v->record_size;
    u32 sectors = v->record_size / v->bytes_per_sector;
    if (!sectors) return 0;

    u32 sector;
    if (recs_per_cluster >= 1) {
        u32 vcn    = num / recs_per_cluster;
        u32 inner  = num % recs_per_cluster;
        u32 lcn, left; int sparse;
        if (!run_lookup(v->mft_runs, v->mft_run_count, vcn, &lcn, &left, &sparse))
            return 0;
        if (sparse) return 0;
        sector = lcn * v->sectors_per_cluster
               + inner * (v->record_size / v->bytes_per_sector);
    } else {
        /* a record spans several clusters */
        u32 clusters_per_rec = v->record_size / v->cluster_size;
        u32 vcn = num * clusters_per_rec;
        u32 lcn, left; int sparse;
        if (!run_lookup(v->mft_runs, v->mft_run_count, vcn, &lcn, &left, &sparse))
            return 0;
        if (sparse) return 0;
        sector = lcn * v->sectors_per_cluster;
    }

    if (!ntfs_read_sectors(v, sector, sectors, dst)) return 0;
    if (memcmp(dst, "FILE", 4) != 0) return 0;
    if (!apply_fixup(v, dst, v->record_size)) return 0;
    return 1;
}

/* Locate an attribute inside a record already in memory. Returns a
   pointer to the attribute header, or NULL. */
static u8 *find_attr_in(u8 *rec, u32 rec_size, u32 type, int skip) {
    u16 first;
    memcpy(&first, rec + 0x14, 2);
    if (first >= rec_size) return NULL;

    u8 *p = rec + first;
    int seen = 0;

    while (p + 8 <= rec + rec_size) {
        u32 atype, alen;
        memcpy(&atype, p, 4);
        if (atype == AT_END) break;
        memcpy(&alen, p + 4, 4);
        if (!alen || p + alen > rec + rec_size) break;

        if (atype == type) {
            if (seen == skip) return p;
            seen++;
        }
        p += alen;
    }
    return NULL;
}

/* Copy the value of a resident attribute. */
static u32 resident_value(u8 *attr, u8 **out) {
    u32 vlen;
    u16 voff;
    memcpy(&vlen, attr + 0x10, 4);
    memcpy(&voff, attr + 0x14, 2);
    *out = attr + voff;
    return vlen;
}

/* Find an attribute, following $ATTRIBUTE_LIST into extension records
   when the base record does not hold it. `rec` is the base record and
   is left untouched; a found attribute may live in `ext` instead. */
static u8 *find_attr_ext(ntfs_vol_t *v, u8 *rec, u32 type, u8 *ext, u8 **owner) {
    u8 *a = find_attr_in(rec, v->record_size, type, 0);
    if (a) { *owner = rec; return a; }

    u8 *list = find_attr_in(rec, v->record_size, AT_ATTR_LIST, 0);
    if (!list) return NULL;

    /* Only a resident attribute list is handled; a non-resident one
       means a pathologically fragmented file. */
    if (list[0x08]) return NULL;

    u8 *val;
    u32 vlen = resident_value(list, &val);
    u32 off  = 0;

    while (off + 0x1A <= vlen) {
        u32 etype;
        u16 elen;
        memcpy(&etype, val + off, 4);
        memcpy(&elen,  val + off + 4, 2);
        if (!elen || off + elen > vlen) break;

        if (etype == type) {
            u32 ref_lo;
            memcpy(&ref_lo, val + off + 0x10, 4);   /* low 32 bits of the reference */
            if (ref_lo && read_mft_record(v, ref_lo, ext)) {
                u8 *b = find_attr_in(ext, v->record_size, type, 0);
                if (b) { *owner = ext; return b; }
            }
        }
        off += elen;
    }
    return NULL;
}

/* ---------- reading attribute contents ---------- */

/* Read `max` bytes at `off` from an attribute (resident or not). */
static int read_attr(ntfs_vol_t *v, u8 *attr, u32 off, void *buf, u32 max,
                     u32 *total_size) {
    u16 flags;
    memcpy(&flags, attr + 0x0C, 2);

    if (flags & (ATTR_COMPRESSED | ATTR_ENCRYPTED)) return -2;  /* unsupported */

    if (!attr[0x08]) {                       /* resident */
        u8 *val;
        u32 vlen = resident_value(attr, &val);
        if (total_size) *total_size = vlen;
        if (off >= vlen) return 0;
        u32 n = vlen - off;
        if (n > max) n = max;
        memcpy(buf, val + off, n);
        return (int)n;
    }

    u32 real_lo;
    memcpy(&real_lo, attr + 0x30, 4);        /* real size, low 32 bits */
    if (total_size) *total_size = real_lo;
    if (off >= real_lo) return 0;

    u32 want = real_lo - off;
    if (want > max) want = max;

    u16 run_off;
    memcpy(&run_off, attr + 0x20, 2);
    u32 alen;
    memcpy(&alen, attr + 0x04, 4);
    u32 start_vcn;
    memcpy(&start_vcn, attr + 0x10, 4);

    /* 192 runs are 3 KiB - too much for a task stack, and read_attr is
       called from inside walk_index. One shared array is safe because
       reads never nest. */
    static ntfs_run_t runs[NTFS_MAX_RUNS];
    int nruns = parse_runlist(attr + run_off, attr + alen, start_vcn,
                              runs, NTFS_MAX_RUNS);
    if (!nruns) return -1;

    u8 *dst = (u8 *)buf;
    u32 done = 0;

    while (done < want) {
        u32 pos   = off + done;
        u32 vcn   = pos / v->cluster_size;
        u32 inner = pos % v->cluster_size;

        u32 lcn, left; int sparse;
        if (!run_lookup(runs, nruns, vcn, &lcn, &left, &sparse)) break;

        u32 n = want - done;
        u32 avail = v->cluster_size - inner;
        if (n > avail) n = avail;

        if (sparse) {
            memset(dst + done, 0, n);         /* a hole reads as zeroes */
        } else {
            if (!read_clusters(v, lcn, 1, sec_buf)) break;
            memcpy(dst + done, sec_buf + inner, n);
        }
        done += n;
    }
    return (int)done;
}

/* ---------- names ---------- */

static u32 utf8_put3(char *d, u32 cp) {
    if (cp < 0x80)  { d[0] = (char)cp; return 1; }
    if (cp < 0x800) { d[0] = (char)(0xC0 | (cp >> 6));
                      d[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    d[0] = (char)(0xE0 | (cp >> 12));
    d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    d[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static void utf16_to_utf8(const u8 *src, u32 chars, char *dst, u32 dstsz) {
    u32 n = 0;
    for (u32 i = 0; i < chars && n + 4 < dstsz; i++) {
        u16 u;
        memcpy(&u, src + i * 2, 2);
        if (!u) break;
        n += utf8_put3(dst + n, u);
    }
    dst[n] = 0;
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_eq_ci(const char *a, const char *b) {
    while (*a && *b) { if (up(*a) != up(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* ---------- directory indexes ---------- */

/* Everything the caller wants to know about one index entry. */
typedef struct {
    u32  mft_ref;
    char name[VFS_MAX_NAME];
    u32  size;
    u8   is_dir, is_hidden, is_system, is_readonly;
    u16  year; u8 month, day, hour, min;
} ntfs_entry_t;

/* Decode one index entry; returns its length, or 0 to stop. */
static u32 decode_index_entry(const u8 *e, ntfs_entry_t *out, int *is_last) {
    u16 elen, klen, eflags;
    memcpy(&elen,   e + 0x08, 2);
    memcpy(&klen,   e + 0x0A, 2);
    memcpy(&eflags, e + 0x0C, 2);

    *is_last = (eflags & 0x02) ? 1 : 0;
    if (!elen) return 0;
    if (*is_last || klen < 0x42) return elen;

    const u8 *fn = e + 0x10;                  /* the $FILE_NAME key */

    u32 ref_lo;
    memcpy(&ref_lo, e, 4);
    out->mft_ref = ref_lo;

    u32 fflags;
    memcpy(&fflags, fn + 0x38, 4);
    out->is_dir      = (fflags & 0x10000000u) ? 1 : 0;
    out->is_readonly = (fflags & 0x0001) ? 1 : 0;
    out->is_hidden   = (fflags & 0x0002) ? 1 : 0;
    out->is_system   = (fflags & 0x0004) ? 1 : 0;

    u32 rsize;
    memcpy(&rsize, fn + 0x30, 4);             /* real size, low 32 bits */
    out->size = out->is_dir ? 0 : rsize;

    u8 nlen  = fn[0x40];
    u8 nspace = fn[0x41];
    /* namespace 2 is the DOS 8.3 alias of a name we already list */
    if (nspace == 2) { out->name[0] = 0; return elen; }

    utf16_to_utf8(fn + 0x42, nlen, out->name, sizeof(out->name));

    /* Timestamps are 100 ns ticks since 1601; converting needs a 64-bit
       division, which this kernel avoids, so they are left empty. */
    out->year = 0;
    return elen;
}

/* Walk every entry of a directory. `cb` returns 0 to stop. */
typedef int (*idx_cb_t)(void *ctx, ntfs_entry_t *e);

static int walk_index(ntfs_vol_t *v, u32 dir_ref, idx_cb_t cb, void *ctx) {
    static u8 base[NTFS_REC_MAX];
    static u8 ext[NTFS_REC_MAX];

    if (!read_mft_record(v, dir_ref, base)) return 0;

    u8 *owner = base;
    u8 *root  = find_attr_ext(v, base, AT_INDEX_ROOT, ext, &owner);
    if (!root) return 0;

    /* $INDEX_ROOT is always resident: a header plus the top entries. */
    u8 *val;
    u32 vlen = resident_value(root, &val);
    if (vlen < 0x20) return 0;

    u32 entries_off;
    memcpy(&entries_off, val + 0x10, 4);       /* relative to the node header */
    u32 entries_end;
    memcpy(&entries_end, val + 0x14, 4);

    u32 p = 0x10 + entries_off;
    u32 e = 0x10 + entries_end;
    if (e > vlen) e = vlen;

    while (p + 0x10 <= e) {
        ntfs_entry_t ent;
        memset(&ent, 0, sizeof(ent));
        int last = 0;
        u32 l = decode_index_entry(val + p, &ent, &last);
        if (!l) break;
        if (!last && ent.name[0]) { if (!cb(ctx, &ent)) return 1; }
        if (last) break;
        p += l;
    }

    /* Larger directories continue in $INDEX_ALLOCATION: a sequence of
       INDX blocks, each with its own fixup. */
    u8 *owner2 = base;
    u8 *alloc  = find_attr_ext(v, base, AT_INDEX_ALLOC, ext, &owner2);
    if (!alloc || !alloc[0x08]) return 1;

    u32 alloc_size;
    memcpy(&alloc_size, alloc + 0x30, 4);

    u32 blk = v->index_size;
    if (blk > NTFS_IDX_MAX) return 1;

    for (u32 off = 0; off + blk <= alloc_size; off += blk) {
        int got = read_attr(v, alloc, off, idx_buf, blk, NULL);
        if (got < (int)blk) break;
        if (memcmp(idx_buf, "INDX", 4) != 0) continue;
        if (!apply_fixup(v, idx_buf, blk)) continue;

        u32 eoff, eend;
        memcpy(&eoff, idx_buf + 0x18, 4);      /* relative to 0x18 */
        memcpy(&eend, idx_buf + 0x1C, 4);

        u32 q = 0x18 + eoff;
        u32 qe = 0x18 + eend;
        if (qe > blk) qe = blk;

        while (q + 0x10 <= qe) {
            ntfs_entry_t ent;
            memset(&ent, 0, sizeof(ent));
            int last = 0;
            u32 l = decode_index_entry(idx_buf + q, &ent, &last);
            if (!l) break;
            if (!last && ent.name[0]) { if (!cb(ctx, &ent)) return 1; }
            if (last) break;
            q += l;
        }
    }
    return 1;
}

/* ---------- path lookup ---------- */

typedef struct {
    const char  *want;
    int          found;
    ntfs_entry_t ent;
} nfind_t;

static int nfind_cb(void *ctx, ntfs_entry_t *e) {
    nfind_t *f = (nfind_t *)ctx;
    if (name_eq_ci(e->name, f->want)) { f->found = 1; f->ent = *e; return 0; }
    return 1;
}

/* Resolve a path to an MFT reference. Returns 1 on success, and sets
   *is_dir. The root directory is reference 5. */
static int ntfs_resolve(ntfs_vol_t *v, const char *path, u32 *ref, int *is_dir,
                        u32 *size) {
    u32 cur = MFT_REC_ROOT;
    int dir = 1;
    u32 sz  = 0;

    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        char comp[VFS_MAX_NAME];
        u32 n = 0;
        while (*p && *p != '/' && n < sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        if (!n) break;

        if (!dir) return 0;

        nfind_t f;
        f.want = comp; f.found = 0;
        memset(&f.ent, 0, sizeof(f.ent));
        walk_index(v, cur, nfind_cb, &f);
        if (!f.found) return 0;

        cur = f.ent.mft_ref;
        dir = f.ent.is_dir;
        sz  = f.ent.size;
    }

    if (ref)    *ref = cur;
    if (is_dir) *is_dir = dir;
    if (size)   *size = sz;
    return 1;
}

/* ---------- VFS operations ---------- */

typedef struct {
    int index, want, done;
    vfs_dirent_t *out;
} nlist_t;

static int nlist_cb(void *ctx, ntfs_entry_t *e) {
    nlist_t *l = (nlist_t *)ctx;

    /* metadata files ($MFT, $LogFile, ...) live in the root; hide them */
    if (e->name[0] == '$' && e->mft_ref < 16) return 1;
    if (e->name[0] == '.' && e->name[1] == 0) return 1;

    if (l->index == l->want) {
        memset(l->out, 0, sizeof(*l->out));
        strncpy(l->out->name, e->name, VFS_MAX_NAME);
        l->out->size        = e->size;
        l->out->is_dir      = e->is_dir;
        l->out->is_hidden   = e->is_hidden;
        l->out->is_system   = e->is_system;
        l->out->is_readonly = e->is_readonly;
        l->done = 1;
        return 0;
    }
    l->index++;
    return 1;
}

static int ntfs_op_readdir(void *fs, const char *path, int index, vfs_dirent_t *out) {
    ntfs_vol_t *v = (ntfs_vol_t *)fs;
    u32 ref; int dir;
    if (!ntfs_resolve(v, path, &ref, &dir, NULL)) return 0;
    if (!dir) return 0;

    nlist_t l = { 0, index, 0, out };
    walk_index(v, ref, nlist_cb, &l);
    return l.done;
}

static int ntfs_op_stat(void *fs, const char *path) {
    ntfs_vol_t *v = (ntfs_vol_t *)fs;
    u32 ref; int dir;
    if (!ntfs_resolve(v, path, &ref, &dir, NULL)) return 0;
    return dir ? 2 : 1;
}

static u32 ntfs_op_size(void *fs, const char *path) {
    ntfs_vol_t *v = (ntfs_vol_t *)fs;
    u32 ref, sz; int dir;
    if (!ntfs_resolve(v, path, &ref, &dir, &sz)) return 0;
    if (dir) return 0;

    /* The index gives a size, but the $DATA attribute is authoritative. */
    if (!read_mft_record(v, ref, rec_buf)) return sz;
    static u8 ext[NTFS_REC_MAX];
    u8 *owner = rec_buf;
    u8 *data  = find_attr_ext(v, rec_buf, AT_DATA, ext, &owner);
    if (!data) return sz;

    if (!data[0x08]) {
        u32 vlen;
        memcpy(&vlen, data + 0x10, 4);
        return vlen;
    }
    u32 real;
    memcpy(&real, data + 0x30, 4);
    return real;
}

static int ntfs_op_read(void *fs, const char *path, u32 off, void *buf, u32 max) {
    ntfs_vol_t *v = (ntfs_vol_t *)fs;
    u32 ref; int dir;
    if (!ntfs_resolve(v, path, &ref, &dir, NULL)) return -1;
    if (dir) return -1;

    if (!read_mft_record(v, ref, rec_buf)) return -1;

    static u8 ext[NTFS_REC_MAX];
    u8 *owner = rec_buf;
    u8 *data  = find_attr_ext(v, rec_buf, AT_DATA, ext, &owner);
    if (!data) return -1;

    return read_attr(v, data, off, buf, max, NULL);
}

static void ntfs_op_space(void *fs, u32 *total_kb, u32 *free_kb) {
    ntfs_vol_t *v = (ntfs_vol_t *)fs;

    /* 32-bit arithmetic only: divide by 512 first, then scale. */
    if (total_kb) *total_kb = (v->total_sectors / 2) * (v->bytes_per_sector / 512);

    if (!free_kb) return;
    *free_kb = 0;

    /* $Bitmap (record 6) has one bit per cluster: count the zeroes. */
    if (!read_mft_record(v, 6, rec_buf)) return;
    static u8 ext[NTFS_REC_MAX];
    u8 *owner = rec_buf;
    u8 *data  = find_attr_ext(v, rec_buf, AT_DATA, ext, &owner);
    if (!data) return;

    static const u8 bits[16] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};
    static u8 chunk[512];
    u32 freec = 0, off = 0;

    /* Reading the whole bitmap over PIO takes seconds on a large volume.
       32 KiB covers 256 Mi clusters worth of accounting and is plenty
       for an indicator; the value is a hint, not an allocation. */
    for (int i = 0; i < 64; i++) {
        int got = read_attr(v, data, off, chunk, sizeof(chunk), NULL);
        if (got <= 0) break;
        for (int k = 0; k < got; k++) {
            u8 b = (u8)~chunk[k];
            freec += bits[b & 0x0F] + bits[b >> 4];
        }
        off += (u32)got;
        if ((u32)got < sizeof(chunk)) break;
    }
    *free_kb = (v->cluster_size >= 1024)
               ? freec * (v->cluster_size / 1024)
               : freec / (1024 / v->cluster_size);
}

static const vfs_ops_t ntfs_ops = {
    ntfs_op_readdir, ntfs_op_read, ntfs_op_size, ntfs_op_stat,
    NULL, NULL, NULL,                     /* read-only, and honest about it */
    ntfs_op_space
};

/* ---------- probe and mount ---------- */

static int parse_boot(const u8 *b, ntfs_vol_t *v) {
    if (memcmp(b + 3, "NTFS    ", 8) != 0) return 0;

    u16 bps;
    memcpy(&bps, b + 0x0B, 2);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;

    u8 spc = b[0x0D];
    if (!spc || (spc & (spc - 1))) return 0;

    u32 total_lo;
    memcpy(&total_lo, b + 0x28, 4);
    if (!total_lo) return 0;

    u32 mft_lo;
    memcpy(&mft_lo, b + 0x30, 4);

    v->bytes_per_sector    = bps;
    v->sectors_per_cluster = spc;
    v->cluster_size        = bps * spc;
    v->total_sectors       = total_lo;
    v->mft_lcn             = mft_lo;

    /* A positive value counts clusters, a negative one is a power of two. */
    i8 cpr = (i8)b[0x40];
    v->record_size = (cpr > 0) ? (u32)cpr * v->cluster_size : (1u << (u32)(-cpr));

    i8 cpi = (i8)b[0x44];
    v->index_size = (cpi > 0) ? (u32)cpi * v->cluster_size : (1u << (u32)(-cpi));

    if (v->record_size < 256 || v->record_size > NTFS_REC_MAX) return 0;
    if (v->index_size  < 256 || v->index_size  > NTFS_IDX_MAX) return 0;
    if (v->cluster_size > 4096) return 0;      /* sec_buf holds one cluster */
    return 1;
}

int ntfs_probe(int disk, u32 lba_start, u32 lba_count, char *label) {
    static u8 b[512];
    static ntfs_vol_t tmp;
    (void)lba_count;
    if (!disk_read(disk, lba_start, 1, b)) return 0;

    memset(&tmp, 0, sizeof(tmp));
    if (!parse_boot(b, &tmp)) return 0;
    if (label) label[0] = 0;
    return 1;
}

/* Read the volume label from $Volume (record 3). */
static void read_label(ntfs_vol_t *v) {
    strncpy(v->label, T("NTFS volume", "том NTFS"), sizeof(v->label));

    if (!read_mft_record(v, MFT_REC_VOLUME, rec_buf)) return;
    u8 *a = find_attr_in(rec_buf, v->record_size, AT_VOLUME_NAME, 0);
    if (!a || a[0x08]) return;

    u8 *val;
    u32 vlen = resident_value(a, &val);
    if (!vlen) return;
    utf16_to_utf8(val, vlen / 2, v->label, sizeof(v->label));
}

int ntfs_mount(const char *name, int disk, u32 lba_start, u32 lba_count) {
    static u8 b[512];
    int slot = -1;
    for (int i = 0; i < NTFS_MAX_VOLS; i++) if (!ntfs_vols[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    ntfs_vol_t *v = &ntfs_vols[slot];
    memset(v, 0, sizeof(*v));
    v->disk = disk;
    v->lba  = lba_start;

    if (!disk_read(disk, lba_start, 1, b)) return -1;
    if (!parse_boot(b, v)) return -1;
    (void)lba_count;

    /* Bootstrap: the MFT describes itself. Read its first record using
       the cluster from the boot sector, then take the real runlist of
       $DATA so the rest of the table becomes reachable. */
    v->mft_runs[0].vcn    = 0;
    v->mft_runs[0].lcn    = v->mft_lcn;
    v->mft_runs[0].len    = 1;
    v->mft_runs[0].sparse = 0;
    v->mft_run_count      = 1;

    if (!read_mft_record(v, 0, rec_buf)) return -1;

    u8 *data = find_attr_in(rec_buf, v->record_size, AT_DATA, 0);
    if (!data || !data[0x08]) return -1;       /* $MFT is always non-resident */

    u16 run_off;
    memcpy(&run_off, data + 0x20, 2);
    u32 alen;
    memcpy(&alen, data + 0x04, 4);

    v->mft_run_count = parse_runlist(data + run_off, data + alen, 0,
                                     v->mft_runs, NTFS_MAX_RUNS);
    if (!v->mft_run_count) return -1;

    read_label(v);

    /* sanity check: the root directory must be readable */
    if (!read_mft_record(v, MFT_REC_ROOT, rec_buf)) return -1;

    v->used = 1;
    return vfs_mount(name, FS_NTFS, &ntfs_ops, v, disk, lba_start,
                     v->total_sectors, v->label, 0);
}
