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
 *  Basic Multilingual Plane are not accepted for new names. Unsupported
 *  or overlong existing LFNs fall back to their unambiguous 8.3 alias.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"
#include "sector_tx.h"

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
    u32 serial, backup_fsinfo, extent, journal, journal_count, generation;
    int error, fenced, writing, active_fat, mirrored;
    /* File WAL uses relative target LBAs and a virtual, disjoint journal range. */
    u32 wal_sector,wal_sectors,wal_cluster,wal_clusters,wal_dir,wal_off;
    u8 wal_entry[32];
    int file_wal,overlay;
    u8   used;
} fat_vol_t;

static fat_vol_t fat_vols[FAT_MAX_VOLS];
static sector_tx_t fat_tx; /* Single filesystem writer; 24 KiB undo in BSS. */
static void fat_meta_begin(fat_vol_t *);
void fat_reset_mounts(void) { fat_meta_begin(0);memset(fat_vols, 0, sizeof(fat_vols)); }

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

static void fence(fat_vol_t *v);
static int live(fat_vol_t *v) { return (!blk_is_usb(v->disk) && !blk_is_ahci(v->disk)) || (v->generation && blk_generation(v->disk)==v->generation); }
#define FAT_WAL_VIRTUAL 0xFFFFFE00u
static int wal_overlap(fat_vol_t *v,u32 sec,u32 count) {
    return v->file_wal && sec<v->wal_sector+v->wal_sectors && v->wal_sector<sec+count;
}
static int wal_target(fat_vol_t *v,u32 sec) {
    return sec<v->extent && !wal_overlap(v,sec,1) &&
        (sec>=v->reserved_sectors || sec==v->fsinfo_sector || sec==v->backup_fsinfo) && sec!=0;
}
static int tx_map(fat_vol_t *v,u32 *lba) {
    if(!v->file_wal) return 1;
    if(*lba>=FAT_WAL_VIRTUAL && *lba-FAT_WAL_VIRTUAL<STX_MAX+1)
        *lba=v->wal_sector+1+(*lba-FAT_WAL_VIRTUAL);
    else if(!wal_target(v,*lba)) return 0;
    *lba+=v->lba;return 1;
}
static int tx_read(void *ctx,uint32_t lba,void *buf) {
    fat_vol_t *v=ctx;return live(v) && tx_map(v,&lba)?blk_read(v->disk,lba,1,buf):-1;
}
static int tx_write(void *ctx,uint32_t lba,const void *buf) {
    fat_vol_t *v=ctx;
    if(v->file_wal && lba==v->wal_dir && memcmp((const u8 *)buf+v->wal_off,v->wal_entry,32)) return -1;
    return live(v) && tx_map(v,&lba)?blk_write(v->disk,lba,1,buf):-1;
}
static int tx_sync(void *ctx) { fat_vol_t *v=ctx;return live(v)?blk_sync(v->disk):-1; }
static stx_io_t tx_io(fat_vol_t *v) {
    stx_io_t io={v,tx_read,tx_write,tx_sync,v->file_wal?0:v->lba,v->extent,
                 v->journal,v->journal_count,v->serial};return io;
}
#include "fat_meta.h"
static int fat_read_sectors(fat_vol_t *v,u32 sec,u32 count,void *buf) {
    u32 factor=v->bytes_per_sector/512;
    if(!live(v) || v->fenced || v->error || sec>=v->total_sectors || count>v->total_sectors-sec || count>64/factor) {
        v->error=1;return 0;
    }
    if(count==1 && fat_meta_eligible(v,sec)) {
        struct fat_meta_entry *e=fat_meta_fetch(v,sec,1);
        if(!e) return 0;
        memcpy(buf,e->data,v->bytes_per_sector);
    } else if(blk_read(v->disk,v->lba+sec*factor,(u8)(count*factor),buf)<0) { v->error=1;return 0; }
    if(v->overlay) for(u32 i=0;i<count;i++) for(unsigned j=0;j<fat_tx.n;j++)
        if(fat_tx.lba[j]==sec+i) memcpy((u8 *)buf+i*512,fat_tx.old[j],512);
    return 1;
}
/* Only fresh, unreferenced COW bodies use the unjournalled path. */
static int fat_write_data(fat_vol_t *v,u32 sec,u32 count,const void *buf) {
    u32 factor=v->bytes_per_sector/512;
    if(!live(v) || !v->writing || v->fenced || v->error || sec>=v->total_sectors || count>v->total_sectors-sec || count>64/factor || wal_overlap(v,sec,count) ||
       blk_write(v->disk,v->lba+sec*factor,(u8)(count*factor),buf)<0) {
        v->error=1;return 0;
    }
    return 1;
}
static int fat_write_sectors(fat_vol_t *v,u32 sec,u32 count,const void *buf) {
    u32 factor=v->bytes_per_sector/512;
    if(!live(v) || !v->writing || v->fenced || v->error || sec>=v->total_sectors || count>v->total_sectors-sec) {
        v->error=1;return 0;
    }
    for(u32 j=0;j<count;j++) {
        const u8 *data=(const u8 *)buf+j*v->bytes_per_sector;
        if(fat_meta_eligible(v,sec+j)) {
            struct fat_meta_entry *e=fat_meta_fetch(v,sec+j,0);
            if(!e) return 0;
            memcpy(e->data,data,v->bytes_per_sector);e->dirty=1;
        } else {
            /* Publish directory metadata only after pending chain changes. */
            if(!fat_meta_flush(v)) return 0;
            for(u32 i=0;i<factor;i++)
                if(stx_write(&fat_tx,(v->file_wal?0:v->lba)+(sec+j)*factor+i,data+i*512)<0) {
                    v->error=1;return 0;
                }
        }
    }
    return 1;
}
static int io_enter(fat_vol_t *v) {
    if(!live(v)) fence(v);
    if(v->fenced) return 0;
    if(!v->writing) v->error=0;
    return !v->error;
}

static u32 cluster_to_sector(fat_vol_t *v, u32 clu) {
    return v->first_data_sector + (clu - 2) * v->sectors_per_cluster;
}

/* ---------- the FAT itself ---------- */

static u32 fat_get(fat_vol_t *v, u32 clu) {
    if(clu<2 || clu-2>=v->total_clusters) { v->error=1;return 0x0FFFFFF7u; }
    u8 *sec = buf_get;
    u32 off = v->is_fat32 ? clu * 4 : clu * 2;
    u32 s   = v->fat_start + (u32)v->active_fat*v->fat_size + off / v->bytes_per_sector;
    u32 o   = off % v->bytes_per_sector;

    if (!fat_read_sectors(v, s, 1, sec)) return 0x0FFFFFF7u;

    if (v->is_fat32) {
        u32 val;
        memcpy(&val, sec + o, 4);
        return val & 0x0FFFFFFFu;
    }
    u16 val;
    memcpy(&val, sec + o, 2);
    return val;
}

static int fat_set(fat_vol_t *v,u32 clu,u32 val) {
    if(clu<2 || clu-2>=v->total_clusters || (v->file_wal && clu>=v->wal_cluster && clu-v->wal_cluster<v->wal_clusters)) { v->error=1;return 0; }
    u32 off=clu*(v->is_fat32?4u:2u),o=off%v->bytes_per_sector;
    u32 s=v->fat_start+off/v->bytes_per_sector,original=0;
    for(u32 f=0;f<v->num_fats;f++) {
        if(!v->mirrored && f!=(u32)v->active_fat) continue;
        if(!fat_read_sectors(v,s+f*v->fat_size,1,buf_set)) return 0;
        u32 old=0;memcpy(&old,buf_set+o,v->is_fat32?4:2);
        u32 low=old&(v->is_fat32?0x0FFFFFFFu:0xFFFFu);
        if(f==0 || !v->mirrored) original=low;
        else if(low!=original) { v->error=1;return 0; } /* never repair mismatched FATs during save */
        u32 nv=v->is_fat32?(old&0xF0000000u)|(val&0x0FFFFFFFu):val&0xFFFFu;
        memcpy(buf_set+o,&nv,v->is_fat32?4:2);
        if(!fat_write_sectors(v,s+f*v->fat_size,1,buf_set)) return 0;
    }
    return 1;
}

static int fat_is_eoc(fat_vol_t *v, u32 clu) {
    return v->is_fat32 ? (clu >= FAT_EOC32) : (clu >= FAT_EOC16);
}

/* FAT32 keeps a cached free-cluster count in the FSInfo sector. It is
   only a hint, but Windows and fsck report a "wrong free count" when it
   drifts, so every allocation and release adjusts it. */
static int fsinfo_adjust(fat_vol_t *v,i32 delta) {
    if(!v->is_fat32 || !v->fsinfo_sector) return 1;
    u32 freec=~0u;
    for(unsigned copy=0;copy<(v->backup_fsinfo?2u:1u);copy++) {
        u32 at=copy?v->backup_fsinfo:v->fsinfo_sector;
        if(!fat_read_sectors(v,at,1,buf_fsinfo)) return 0;
        u32 sig1,sig2,trail;
        memcpy(&sig1,buf_fsinfo,4);memcpy(&sig2,buf_fsinfo+484,4);memcpy(&trail,buf_fsinfo+508,4);
        if(sig1!=0x41615252u || sig2!=0x61417272u || trail!=0xAA550000u) { v->error=1;return 0; }
        if(!copy) {
            memcpy(&freec,buf_fsinfo+488,4);
            if(freec!=~0u) {
                if(freec>v->total_clusters || (delta<0 && !freec) || (delta>0 && freec==v->total_clusters)) {
                    v->error=1;return 0;
                }
                freec=delta<0?freec-1:freec+1;
            }
        }
        memcpy(buf_fsinfo+488,&freec,4);
        u32 unknown=~0u;memcpy(buf_fsinfo+492,&unknown,4);
        if(!fat_write_sectors(v,at,1,buf_fsinfo)) return 0;
    }
    return 1;
}
static u32 fat_alloc_cluster(fat_vol_t *v) {
    u32 loaded=~0u,entry_bytes=v->is_fat32?4u:2u;
    for(u32 c=2;c<v->total_clusters+2;c++) {
        u32 off=c*entry_bytes,s=v->fat_start+(u32)v->active_fat*v->fat_size+off/v->bytes_per_sector;
        /* Read each FAT sector once per scan, not once per entry. */
        if(s!=loaded) { if(!fat_read_sectors(v,s,1,buf_get)) return 0;loaded=s; }
        u32 val=0;memcpy(&val,buf_get+off%v->bytes_per_sector,entry_bytes);
        val&=v->is_fat32?0x0FFFFFFFu:0xFFFFu;
        if(val==FAT_FREE) {
            if(!fat_set(v,c,v->is_fat32?0x0FFFFFFFu:0xFFFFu) || !fsinfo_adjust(v,-1)) return 0;
            return c;
        }
    }
    return 0;
}
static u32 chain_next(fat_vol_t *v,u32 c) {
    u32 n=fat_get(v,c);
    if(!fat_is_eoc(v,n) && (n<2 || n-2>=v->total_clusters || n>=(v->is_fat32?0x0FFFFFF0u:0xFFF0u))) v->error=1;
    return n;
}
/* Floyd detects cycles before freeing or traversing a damaged chain. */
static int chain_valid(fat_vol_t *v,u32 c) {
    if(!c) return 1;
    u32 slow=c,fast=c;
    for(u32 guard=0;guard<v->total_clusters;guard++) {
        slow=chain_next(v,slow);fast=chain_next(v,fast);
        if(v->error) return 0;
        if(fat_is_eoc(v,fast)) return 1;
        fast=chain_next(v,fast);if(v->error) return 0;
        if(fat_is_eoc(v,fast)) return 1;
        if(slow==fast) break;
    }
    v->error=1;return 0;
}
static int fat_free_chain(fat_vol_t *v,u32 clu) {
    if(!chain_valid(v,clu)) return 0;
    while(clu>=2 && !fat_is_eoc(v,clu)) {
        u32 next=chain_next(v,clu);
        if(v->error || !fat_set(v,clu,FAT_FREE) || !fsinfo_adjust(v,+1)) return 0;
        clu=next;
    }
    return 1;
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
    const u8 *p=(const u8 *)*s;u32 c=*p++;*s=(const char *)p;
    if(c<0x80) return c;
    if(c>=0xC2 && c<=0xDF && p[0] && (p[0]&0xC0)==0x80) {
        *s=(const char *)(p+1);return ((c&31)<<6)|(p[0]&63);
    }
    if(c>=0xE0 && c<=0xEF && p[0] && (p[0]&0xC0)==0x80 && p[1] && (p[1]&0xC0)==0x80 &&
       !(c==0xE0 && p[0]<0xA0) && !(c==0xED && p[0]>=0xA0)) {
        *s=(const char *)(p+2);return ((c&15)<<12)|((u32)(p[0]&63)<<6)|(p[1]&63);
    }
    return c; /* malformed/OEM byte: consume only that byte, never past NUL */
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

static int short_bad(char c) {
    return c=='.' || c=='+' || c==',' || c==';' || c=='=' || c=='[' || c==']' || c==' ';
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
        if (n < 8) { out[n++] = short_bad(*p)?'_':(u8)upcase(*p);if(short_bad(*p)) lossless=0; }
        else lossless = 0;
    }
    if (!n) { out[0] = '_'; lossless = 0; }

    if (dot) {
        int e = 0;
        for (const char *p = dot + 1; *p; p++) {
            if ((u8)*p >= 0x80) { lossless = 0; continue; }
            if (e < 3) { out[8 + e++] = short_bad(*p)?'_':(u8)upcase(*p);if(short_bad(*p)) lossless=0; }
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

typedef struct { u32 sector,off; } dir_slot_t;
typedef struct { fat_vol_t *v;u32 clu,pos,loaded;int fixed;u8 *buf; } dir_iter_t;
static int dir_init(dir_iter_t *it,fat_vol_t *v,u32 clu,u8 *buf) {
    it->v=v;it->clu=clu;it->pos=0;it->loaded=~0u;it->buf=buf;
    it->fixed=!v->is_fat32 && !clu;
    return it->fixed || (clu>=2 && chain_valid(v,clu));
}
static int dir_next(dir_iter_t *it,dir_slot_t *slot) {
    fat_vol_t *v=it->v;u32 per=v->bytes_per_sector/32;
    if(it->fixed) {
        if(it->pos>=v->root_entries) return 0;
        slot->sector=v->fat_start+v->num_fats*v->fat_size+it->pos/per;
    } else {
        if(it->pos==v->sectors_per_cluster*per) {
            it->clu=chain_next(v,it->clu);it->pos=0;
            if(v->error) return -1;
        }
        if(fat_is_eoc(v,it->clu)) return 0;
        slot->sector=cluster_to_sector(v,it->clu)+it->pos/per;
    }
    slot->off=(it->pos++%per)*32;
    if(it->loaded!=slot->sector) {
        if(!fat_read_sectors(v,slot->sector,1,it->buf)) return -1;
        it->loaded=slot->sector;
    }
    return 1;
}

/* Iterate the entries of a directory cluster chain (or the fixed FAT16
   root). `cb` is called for every real entry; returning 0 stops the
   walk. Long names are assembled here so callers never see them. */
typedef int (*dir_cb_t)(void *ctx, const char *name, fat_dirent_t *de,
                        u32 dir_sector, u32 entry_off);

static int walk_dir(fat_vol_t *v, u32 start_cluster, dir_cb_t cb, void *ctx) {
    u8 *sec = buf_walk;
    if(!io_enter(v) || (start_cluster && !chain_valid(v,start_cluster))) return 0;
    int expected=0;u8 checksum=0;
    int  have_lfn = 0;
    u16  lfn_units[260] = {0};
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
                    int ord=l->order&0x1F,idx=ord-1;
                    if(l->order&0x40) { expected=ord;lfn_max=ord;checksum=l->checksum;have_lfn=1; }
                    if(!have_lfn || ord<1 || ord>20 || ord!=expected || l->type || l->cluster ||
                       l->checksum!=checksum || (l->order&0xA0)) { have_lfn=0;expected=0; }
                    else {
                        u16 *dst=lfn_units+idx*13;
                        memcpy(dst,l->name1,10);memcpy(dst+5,l->name2,12);memcpy(dst+11,l->name3,4);expected--;
                    }
                    continue;
                }

                if (de->attr & FAT_ATTR_LABEL) { have_lfn = 0; continue; }

                char name[VFS_MAX_NAME];
                if (have_lfn && !expected && lfn_max && checksum==lfn_checksum(de->name)) {
                    u32 n = 0;
                    for (int i = 0; i < lfn_max * 13; i++) {
                        u16 u = lfn_units[i];
                        if (u == 0x0000 || u == 0xFFFF) break;
                        u32 bytes=u<0x80?1u:u<0x800?2u:3u;
                        if(n+bytes>=sizeof(name) || (u>=0xD800 && u<=0xDFFF)) { n=0;break; }
                        n += utf8_put(name + n, u);
                    }
                    name[n] = 0;
                    if(!n) name83_to_str(de->name,name);
                } else {
                    name83_to_str(de->name, name);
                }
                have_lfn = 0; lfn_max = 0;


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

static u32 fold(u32 c) {
    if((c>='a' && c<='z') || (c>=0x430 && c<=0x44F)) return c-32;
    return c==0x451?0x401:c;
}
static int streq_ci(const char *a,const char *b) {
    while(*a && *b) if(fold(utf8_get(&a))!=fold(utf8_get(&b))) return 0;
    return *a==0 && *b==0;
}

static int find_cb(void *ctx, const char *name, fat_dirent_t *de,
                   u32 sector, u32 off) {
    find_ctx_t *f = (find_ctx_t *)ctx;
    char alias[13];name83_to_str(de->name,alias);
    if (streq_ci(name, f->want) || streq_ci(alias,f->want)) {
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
    return c & (v->is_fat32?0x0FFFFFFFu:0xFFFFu);
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
        if(*p && *p!='/') return -1;
        comp[n] = 0;
        while (*p == '/') p++;

        find_ctx_t f; f.want = comp; f.found = 0; f.sector = 0; f.off = 0; memset(&f.de, 0, sizeof(f.de));
        if(!walk_dir(v, clu, find_cb, &f)) return -1;
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
    int capacity;
} list_ctx_t;

static int list_cb(void *ctx, const char *name, fat_dirent_t *de,
                   u32 sector, u32 off) {
    list_ctx_t *l = (list_ctx_t *)ctx;
    (void)sector; (void)off;

    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        return 1;                                   /* skip . and .. */

    if (l->index++ >= l->want) {
        vfs_dirent_t *out=l->out+l->done;memset(out,0,sizeof(*out));
        strncpy(out->name, name, VFS_MAX_NAME);
        out->is_dir      = (de->attr & FAT_ATTR_DIR) ? 1 : 0;
        out->is_readonly = (de->attr & FAT_ATTR_RO) ? 1 : 0;
        out->is_hidden   = (de->attr & FAT_ATTR_HIDDEN) ? 1 : 0;
        out->is_system   = (de->attr & FAT_ATTR_SYSTEM) ? 1 : 0;
        out->size        = out->is_dir ? 0 : de->size;
        out->mtime_year  = (u16)(1980 + ((de->mdate >> 9) & 0x7F));
        out->mtime_month = (u8)((de->mdate >> 5) & 0x0F);
        out->mtime_day   = (u8)(de->mdate & 0x1F);
        out->mtime_hour  = (u8)((de->mtime >> 11) & 0x1F);
        out->mtime_min   = (u8)((de->mtime >> 5) & 0x3F);
        return ++l->done<l->capacity;
    }
    return 1;
}

static int fat_readdir_impl(void *fs, const char *path, int index, vfs_dirent_t *out,int count) {
    fat_vol_t *v = (fat_vol_t *)fs;
    fat_dirent_t de;
    u32 clu;

    int found=resolve_path(v,path,&de,NULL,NULL,&clu);
    if(found<0 || (found && !(de.attr&FAT_ATTR_DIR))) return -1;

    list_ctx_t l = { 0, index, out, 0, count };
    if(!walk_dir(v,clu,list_cb,&l)) return -1;
    return l.done;
}

/* Listing still fetches directory sectors through guarded block I/O. */
static int fat_op_readdir_many(void *fs,const char *path,int index,vfs_dirent_t *out,int count) {
    if(!out || index<0 || count<1 || count>128 || index>0x7FFFFFFF-count) return -1;
    fat_vol_t *v=fs;
    if(!io_enter(v)) return -1;
    if(v->writing) return fat_readdir_impl(fs,path,index,out,count);
    fat_meta_begin(v);
    int r=fat_readdir_impl(fs,path,index,out,count);
    fat_meta_begin(0);return r;
}

static int fat_op_readdir(void *fs,const char *path,int index,vfs_dirent_t *out) {
    return fat_op_readdir_many(fs,path,index,out,1);
}

static int fat_op_stat(void *fs, const char *path) {
    fat_vol_t *v = (fat_vol_t *)fs;
    if(!io_enter(v)) return 0;
    fat_dirent_t de;
    int r = resolve_path(v, path, &de, NULL, NULL, NULL);
    if (r < 0) return 0;
    if (r == 0) return 2;                       /* the root is a directory */
    return (de.attr & FAT_ATTR_DIR) ? 2 : 1;
}

static u32 fat_op_size(void *fs, const char *path) {
    fat_vol_t *v = (fat_vol_t *)fs;
    if(!io_enter(v)) return 0;
    fat_dirent_t de;
    if (resolve_path(v, path, &de, NULL, NULL, NULL) != 1) return 0;
    return (de.attr & FAT_ATTR_DIR) ? 0 : de.size;
}

static int fat_read_impl(void *fs, const char *path, u32 off, void *buf, u32 max) {
    fat_vol_t *v = (fat_vol_t *)fs;
    fat_dirent_t de;
    u32 clu;

    if (resolve_path(v, path, &de, NULL, NULL, &clu) != 1) return -1;
    if (de.attr & FAT_ATTR_DIR) return -1;
    if(!chain_valid(v,clu)) return -1;
    if (off >= de.size) return 0;

    u32 want = de.size - off;
    if (want > max) want = max;

    u32 cluster_bytes = v->sectors_per_cluster * v->bytes_per_sector;

    /* skip whole clusters up to the offset */
    u32 skip = off / cluster_bytes;
    for (u32 i = 0; i < skip; i++) {
        if (clu < 2 || fat_is_eoc(v, clu) || v->error) return -1;
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

            if (!fat_read_sectors(v, base + s, 1, sec)) return -1;

            u32 from = (inner > sec_off) ? (inner - sec_off) : 0;
            u32 n    = v->bytes_per_sector - from;
            if (n > want - done) n = want - done;
            memcpy(dst + done, sec + from, n);
            done += n;
        }
        inner = 0;
        clu = fat_get(v, clu);
    }
    return !v->error && done==want?(int)done:-1;
}

/* Cache only internal FAT links within this call. Path and payload sectors
 * still use real block I/O, including USB ancestor/cache guards. */
static int fat_op_read(void *fs,const char *path,u32 off,void *buf,u32 max) {
    fat_vol_t *v=fs;
    if(!io_enter(v)) return -1;
    if(v->writing) return fat_read_impl(fs,path,off,buf,max);
    fat_meta_begin(v);
    int r=fat_read_impl(fs,path,off,buf,max);
    fat_meta_begin(0);return r;
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

    if (free_kb && blk_is_usb(v->disk)) { *free_kb = 0; return; } /* read-only; no slow free-space scan */
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

/* Free logical slots, not an assumed contiguous physical span. Keep an
 * end marker if newly created entries consume the old 00 marker. */
static dir_slot_t selected[22];
static int selected_end;
static int alloc_dir_slots(fat_vol_t *v,u32 dir_cluster,u32 need) {
    if(need>21) return 0;
    dir_iter_t it;dir_slot_t slot;u32 run=0,last=dir_cluster;int end=0;
    if(!dir_init(&it,v,dir_cluster,buf_slots)) return 0;
    selected_end=0;
    for(;;) {
        int r=dir_next(&it,&slot);
        if(r<0) return 0;
        if(!r) {
            if(run>=need) return 1; /* boundary itself terminates the directory */
            if(it.fixed) return 0;
            u32 nc=fat_alloc_cluster(v);if(!nc) return 0;
            memset(buf_io,0,v->bytes_per_sector);
            for(u32 j=0;j<v->sectors_per_cluster;j++)
                if(!fat_write_data(v,cluster_to_sector(v,nc)+j,1,buf_io)) return 0;
            if(!fat_set(v,last,nc)) return 0;
            it.clu=nc;it.pos=0;it.loaded=~0u;end=1;continue;
        }
        last=it.clu;
        if(buf_slots[slot.off]==0) end=1;
        if(end || buf_slots[slot.off]==0xE5) {
            selected[run++]=slot;
            if(run>=need && !end) return 1;
            if(run>need) { selected_end=1;return 1; }
        } else run=0;
    }
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
        u8 base83[8];memcpy(base83,n83,8);
        for (int attempt = 1; attempt < 100; attempt++) {
            char probe[13];
            name83_to_str(n83, probe);
            find_ctx_t f; f.want = probe; f.found = 0; f.sector = 0; f.off = 0; memset(&f.de, 0, sizeof(f.de));
            walk_dir(v, dir_cluster, find_cb, &f);
            if(v->error) return 0;
            if (!f.found) break;
            if(attempt==99) return 0;
            char tail[8];
            ksnprintf(tail, sizeof(tail), "~%d", attempt);
            u32 tl = strlen(tail);
            memcpy(n83,base83,8);
            u32 base=0;while(base<8 && n83[base]!=' ') base++;
            if(base>8-tl) base=8-tl;
            memcpy(n83+base,tail,tl);
            for(u32 i=base+tl;i<8;i++) n83[i]=' ';
        }
    }

    u32 need = lfn_count + 1;
    if (!alloc_dir_slots(v, dir_cluster, need)) return 0;

    u8 sum = lfn_checksum(n83);
    u8 *sec = buf_entry;

    /* Fragments are stored in reverse order, last one first. */
    for (u32 i = 0; i < lfn_count; i++) {
        u32 idx = lfn_count - i;                 /* 1-based */
        u32 s=selected[i].sector,o=selected[i].off;

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
    u32 s=selected[lfn_count].sector,o=selected[lfn_count].off;

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
    if(!fat_write_sectors(v,s,1,sec)) return 0;
    if(selected_end) {
        s=selected[need].sector;o=selected[need].off;
        if(!fat_read_sectors(v,s,1,sec)) return 0;
        memset(sec+o,0,32);
        if(!fat_write_sectors(v,s,1,sec)) return 0;
    }
    return 1;
}

/* Mark an entry and its long-name fragments as deleted. */
static int erase_entry(fat_vol_t *v, u32 dir_cluster, const char *name) {
    find_ctx_t f={0};f.want=name;
    if(!walk_dir(v,dir_cluster,find_cb,&f) || !f.found) return 0;
    dir_iter_t it;dir_slot_t slot,lfn[20];unsigned n=0;int expected=0;
    if(!dir_init(&it,v,dir_cluster,buf_erase)) return 0;
    while(dir_next(&it,&slot)>0) {
        if(slot.sector==f.sector && slot.off==f.off) {
            /* Delete only the checksum/ordinal-validated adjacent chain. */
            if(expected) n=0;
            for(unsigned i=0;i<=n;i++) {
                dir_slot_t x=i<n?lfn[i]:slot;
                if(!fat_read_sectors(v,x.sector,1,buf_erase)) return 0;
                buf_erase[x.off]=0xE5;
                if(!fat_write_sectors(v,x.sector,1,buf_erase)) return 0;
            }
            return 1;
        }
        fat_lfn_t *l=(fat_lfn_t *)(buf_erase+slot.off);
        if(l->attr==FAT_ATTR_LFN && l->order!=0xE5 && l->checksum==lfn_checksum(f.de.name) &&
           !l->type && !l->cluster && (l->order&0x1F)>=1 && (l->order&0x1F)<=20) {
            if(l->order&0x40) { n=0;expected=l->order&0x1F; }
            if(expected==(l->order&0x1F) && n<20) { lfn[n++]=slot;expected--; }
            else { n=0;expected=0; }
        } else { n=0;expected=0; }
    }
    return 0;
}

/* Split "/dir/name" into the parent cluster and the final component. */
static int split_parent(fat_vol_t *v, const char *path, u32 *parent_cluster,
                        char *leaf, u32 leafsz) {
    char dir[VFS_MAX_PATH];
    u32 len = 0;
    while (path[len] && len < sizeof(dir) - 1) { dir[len] = path[len]; len++; }
    if(path[len]) return 0;
    dir[len] = 0;

    /* strip a trailing slash */
    while (len > 1 && dir[len - 1] == '/') dir[--len] = 0;

    i32 cut = -1;
    for (i32 i = (i32)len - 1; i >= 0; i--) if (dir[i] == '/') { cut = i; break; }
    if (cut < 0) return 0;

    u32 n = 0;
    for (u32 i = (u32)cut + 1; dir[i] && n < leafsz - 1; i++) leaf[n++] = dir[i];
    if((u32)cut+1+n<len) return 0;
    leaf[n] = 0;
    if(!n || leaf[n-1]==' ' || leaf[n-1]=='.' || !strcmp(leaf,".") || !strcmp(leaf,"..")) return 0;
    for(u32 j=0;j<n;) {
        u8 c=(u8)leaf[j];
        if(c<32 || c==127 || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|' || c=='\\') return 0;
        u32 bytes=c<0x80?1u:(c>=0xC2 && c<=0xDF)?2u:(c>=0xE0 && c<=0xEF)?3u:0u;
        if(!bytes || bytes>n-j) return 0;
        for(u32 k=1;k<bytes;k++) if(((u8)leaf[j+k]&0xC0)!=0x80) return 0;
        if(bytes==3 && ((c==0xE0 && (u8)leaf[j+1]<0xA0) || (c==0xED && (u8)leaf[j+1]>=0xA0))) return 0;
        j+=bytes;
    }

    dir[cut] = 0;
    const char *parent = (cut == 0) ? "/" : dir;

    fat_dirent_t de;
    u32 clu;
    int resolved=resolve_path(v,parent,&de,NULL,NULL,&clu);
    if(resolved<0 || (resolved==1 && !(de.attr&FAT_ATTR_DIR))) return 0;
    *parent_cluster = clu;
    return 1;
}

static int fat_write_impl(void *fs,const char *path,const void *data,u32 size) {
    fat_vol_t *v=fs;u32 parent;
    char leaf[VFS_MAX_NAME];
    if((size && !data) || size>0x7FFFFFFFu || !split_parent(v,path,&parent,leaf,sizeof(leaf))) return -1;
    fat_dirent_t old;u32 oldsec=0,oldoff=0,oc=0;
    int exists=resolve_path(v,path,&old,&oldsec,&oldoff,NULL)==1;
    if(v->error) return -1;
    if(exists) {
        if(old.attr&(FAT_ATTR_DIR|FAT_ATTR_RO)) return -2;
        oc=entry_cluster(v,&old);if(!chain_valid(v,oc)) return -1;
    }
    u32 first=0,prev=0,done=0;
    while(done<size) {
        u32 c=fat_alloc_cluster(v);if(!c) return v->error?-1:-3;
        if(!first) first=c;else if(!fat_set(v,prev,c)) return -1;
        prev=c;
        for(u32 j=0;j<v->sectors_per_cluster && done<size;j++) {
            u32 n=size-done;if(n>v->bytes_per_sector) n=v->bytes_per_sector;
            memset(buf_io,0,v->bytes_per_sector);memcpy(buf_io,(const u8 *)data+done,n);
            if(!fat_write_data(v,cluster_to_sector(v,c)+j,1,buf_io)) return -1;
            done+=n;
        }
    }
    /* Do not publish a directory entry before the new body/chain is durable. */
    if(!fat_meta_flush(v) || !disk_sync(v->disk)) return -1;
    if(exists) {
        /* Preserve aliases, all LFN slots, attributes and creation timestamps. */
        if(!fat_read_sectors(v,oldsec,1,buf_entry)) return -1;
        old.cluster_lo=(u16)first;old.cluster_hi=(u16)(first>>16);old.size=size;old.attr|=FAT_ATTR_ARCHIVE;
        rtc_time_t tm;rtc_read(&tm);
        old.mdate=(u16)(((tm.year>=1980?tm.year-1980:0)<<9)|((u32)tm.month<<5)|tm.day);
        old.mtime=(u16)(((u32)tm.hour<<11)|((u32)tm.min<<5)|(tm.sec/2));
        memcpy(buf_entry+oldoff,&old,32);
        if(!fat_write_sectors(v,oldsec,1,buf_entry)) return -1;
    } else if(!write_entry(v,parent,leaf,first,size,FAT_ATTR_ARCHIVE)) return -1;
    /* No allocations after this point: old data clusters cannot be reused
       until the operation has committed (or every metadata sector restored). */
    if(oc && !fat_free_chain(v,oc)) return -1;
    return (int)size;
}

static int fat_remove_impl(void *fs, const char *path) {
    fat_vol_t *v = (fat_vol_t *)fs;

    fat_dirent_t de;
    if (resolve_path(v, path, &de, NULL, NULL, NULL) != 1 || (de.attr&FAT_ATTR_RO)) return -1;

    /* refuse to delete a directory that still has entries */
    if (de.attr & FAT_ATTR_DIR) {
        vfs_dirent_t probe;
        list_ctx_t l = { 0, 0, &probe, 0, 1 };
        if(!walk_dir(v,entry_cluster(v,&de),list_cb,&l)) return -1;
        if (l.done) return -4;                    /* not empty */
    }

    u32 parent;
    char leaf[VFS_MAX_NAME];
    if (!split_parent(v, path, &parent, leaf, sizeof(leaf))) return -1;

    u32 c = entry_cluster(v, &de);
    if (c >= 2 && !fat_free_chain(v, c)) return -1;
    return erase_entry(v, parent, leaf) ? 0 : -1;
}

static int fat_mkdir_impl(void *fs, const char *path) {
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
    if (!fat_write_data(v, base, 1, sec)) return -1;

    memset(sec, 0, v->bytes_per_sector);
    for (u32 s = 1; s < v->sectors_per_cluster; s++)
        if (!fat_write_data(v, base + s, 1, sec)) return -1;

    if (!write_entry(v, parent, leaf, c, 0, FAT_ATTR_DIR)) {
        fat_free_chain(v, c);
        return -1;
    }
    return 0;
}

static void fence(fat_vol_t *v) {
    v->fenced=1;
    for(int i=0;i<vfs_volume_count();i++) { vfs_volume_t *m=vfs_volume(i);if(m && m->fs==v) m->writable=0; }
}
static int wal_lease(fat_vol_t *v) {
    return blk_fat_authorize(v->disk,v->lba,v->extent,v->generation)==0;
}
static void wal_release(fat_vol_t *v) {
    if(v->file_wal) blk_fat_revoke(v->disk);
}
static int wal_clean_bit(fat_vol_t *v,int clean) {
    if(!v->file_wal) return 1;
    for(u32 f=0;f<v->num_fats;f++) {
        u32 sec=v->fat_start+f*v->fat_size,n;
        if(!fat_read_sectors(v,sec,1,buf_set)) return 0;
        memcpy(&n,buf_set+4,4);n=clean?(n|0x08000000u):(n&~0x08000000u);
        memcpy(buf_set+4,&n,4);
        if(!fat_write_sectors(v,sec,1,buf_set)) return 0;
    }
    return 1;
}
static int write_begin(fat_vol_t *v) {
    if(!live(v)) fence(v);
    if(v->fenced || v->writing) return 0;
    v->error=0;fat_meta_begin(0);
    if(v->file_wal && !wal_lease(v)) { fence(v);return 0; }
    stx_io_t io=tx_io(v);
    if(stx_begin(&fat_tx,&io)<0) { wal_release(v);fence(v);return 0; }
    v->writing=1;fat_meta_begin(v);
    if(!wal_clean_bit(v,0) || !fat_meta_flush(v)) {
        fat_meta_begin(0);
        if(stx_abort(&fat_tx)<0) fence(v);
        v->writing=0;wal_release(v);return 0;
    }
    return 1;
}
static int write_end(fat_vol_t *v,int result) {
    if(result>=0 && !v->error && wal_clean_bit(v,1) && fat_meta_flush(v) && stx_commit(&fat_tx)==0) {
        fat_meta_begin(0);v->writing=0;wal_release(v);if(!live(v)) { fence(v);return -1; }return result;
    }
    fat_meta_begin(0);
    if(stx_abort(&fat_tx)<0) fence(v);
    v->writing=0;wal_release(v);return result<0?result:-1;
}
static int fat_op_write(void *fs,const char *p,const void *d,u32 n) {
    fat_vol_t *v=fs;if(!write_begin(v)) return -1;
    return write_end(v,fat_write_impl(fs,p,d,n));
}
static int fat_op_remove(void *fs,const char *p) {
    fat_vol_t *v=fs;if(!write_begin(v)) return -1;
    return write_end(v,fat_remove_impl(fs,p));
}
static int fat_op_mkdir(void *fs,const char *p) {
    fat_vol_t *v=fs;if(!write_begin(v)) return -1;
    return write_end(v,fat_mkdir_impl(fs,p));
}
#ifdef FAT_TX_TEST
/* Host fixtures explicitly own this extent. NOT present in normal/lab ISOs;
 * never infer ownership from an unpartitioned gap on someone's USB stick. */
int fat_test_journal(const char *name,u32 first,u32 count,int provision) {
    vfs_volume_t *m=vfs_find(name);if(!m || m->kind!=FS_FAT32) return -1;
    fat_vol_t *v=m->fs;
    if(v->bytes_per_sector!=512 || count>blk_sectors(v->disk) || first>blk_sectors(v->disk)-count) return -1;
    stx_io_t io=tx_io(v);io.journal=first;io.journal_count=count;
    int r=provision?stx_format(&fat_tx,&io):stx_recover(&fat_tx,&io);
    if(r<0) { v->fenced=1;m->writable=0;return -1; }
    v->journal=first;v->journal_count=count;v->fenced=v->error=0;
    return 0;
}
#endif

static const vfs_ops_t fat_ops = {
    fat_op_readdir, fat_op_read, fat_op_size, fat_op_stat,
    fat_op_write, fat_op_remove, fat_op_mkdir, fat_op_space, fat_op_readdir_many, NULL
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
    if(fatsz>(~0u-reserved-root_sectors)/nfats || fatsz>~0u/bps) return 0;
    v->first_data_sector = reserved + nfats * fatsz + root_sectors;
    if (v->first_data_sector >= total) return 0;

    u32 data_sectors = total - v->first_data_sector;
    v->total_clusters = data_sectors / spc;
    if(v->total_clusters<4085 || v->total_clusters>=0x0FFFFFEEu) return 0;

    v->is_fat32 = (v->total_clusters >= 65525);
    if(fatsz*bps/(v->is_fat32?4u:2u)<v->total_clusters+2 || b[510]!=0x55 || b[511]!=0xAA) return 0;
    v->mirrored=1;
    if (v->is_fat32) {
        memcpy(&v->root_cluster, b + 44, 4);
        if(root_ent || fatsz16 || v->root_cluster<2 || v->root_cluster-2>=v->total_clusters || b[42] || b[43]) return 0;
        u16 flags;memcpy(&flags,b+40,2);
        if(flags&0x80) { v->mirrored=0;v->active_fat=flags&15;if((u32)v->active_fat>=nfats) return 0; }
        memcpy(&v->serial,b+67,4);
        u16 fsi;
        memcpy(&fsi, b + 48, 2);
        v->fsinfo_sector = (fsi && fsi != 0xFFFF) ? fsi : 0;
        if(v->fsinfo_sector>=reserved) return 0;
        u16 backup;memcpy(&backup,b+50,2);
        if(v->fsinfo_sector && backup && backup!=0xFFFF) {
            if((u32)backup+v->fsinfo_sector>=reserved) return 0;
            v->backup_fsinfo=backup+v->fsinfo_sector;
        }
        if (kind) *kind = FS_FAT32;
    } else {
        if(!root_ent || !fatsz16) return 0;
        memcpy(&v->serial,b+39,4);
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
    u32 available=blk_sectors(disk);
    if(lba_start>=available || tmp.total_sectors>(available-lba_start)/(tmp.bytes_per_sector/512) ||
       (lba_count && tmp.total_sectors>lba_count/(tmp.bytes_per_sector/512))) return 0;
    return 1;
}

static int lab_prepare(fat_vol_t *v,const u8 *b,u32 partition_count) {
    if(!blk_is_usb(v->disk)) return 0;
    u32 capacity=blk_sectors(v->disk);if(capacity<4096) return 0;
    static u8 marker[512];
    if(blk_read(v->disk,capacity-1024,1,marker)<0) return -1;
    const char magic[]="KVANTOS-M2-FAT-LAB";
    if(memcmp(marker,magic,sizeof(magic))) return 0;
#if KV_STORAGE_LAB
    u32 fields[8],crc;memcpy(fields,marker+64,sizeof(fields));memcpy(&crc,marker+508,4);
    if(crc!=stx_crc32(marker,508) || fields[0]!=capacity || fields[1]!=2) return -1;
    if(fields[2]!=v->lba) return 0; /* Other partitions stay RO. */
    if(!v->is_fat32 || v->bytes_per_sector!=512 || !v->mirrored ||
       fields[3]!=v->extent || partition_count!=v->extent || fields[4]!=v->serial || fields[7]!=stx_crc32(b,512) ||
       fields[5]!=capacity-1023 || fields[6]!=STX_MAX+1 ||
       v->lba>=capacity-1024 || v->extent>capacity-1024-v->lba ||
       fields[6]>capacity-33-fields[5]) return -1;
    v->journal=fields[5];v->journal_count=fields[6];
    if(usb_test_authorize_pair(v->disk-4,v->lba,v->extent,v->journal,v->journal_count)<0) return -1;
    stx_io_t io=tx_io(v);
    if(stx_recover(&fat_tx,&io)<0) {
        usb_test_authorize(v->disk-4,0,0);
        kputs("M2FAT recovery FAILED: namespace withheld\n");return -1;
    }
    kputs("M2FAT recovery complete BEFORE namespace publication\n");return 1;
#else
    (void)b;(void)partition_count;
    kputs("M2FAT lab volume withheld: normal kernel cannot recover it\n");return -1;
#endif
}

#include "fat_wal.h"

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
    u32 available=blk_sectors(disk),factor=v->bytes_per_sector/512;
    if(lba_start>=available || v->total_sectors>(available-lba_start)/factor ||
       (lba_count && v->total_sectors>lba_count/factor)) return -1;
    v->extent=v->total_sectors*factor;
    v->generation=blk_generation(disk);
    int enrolled=lab_prepare(v,b,lba_count);if(enrolled<0) return -1;
    if(!enrolled && wal_prepare(v,b)<0) {
        kputs("FAT owned WAL invalid: namespace withheld (no guessed repair)\n");return -1;
    }

    /* A FAT32 volume label is usually only in the root directory. */
    if (!v->label[0]) strncpy(v->label, T("no label", "без метки"), sizeof(v->label));

    v->used = 1;
    int mounted=vfs_mount(name,kind,&fat_ops,v,disk,lba_start,v->extent,v->label,
#ifdef FAT_TX_TEST
        !v->file_wal
#else
        0
#endif
    );
    if(mounted<0) v->used=0;
#if KV_STORAGE_LAB
    if(enrolled==1) {
        vfs_volume_t *m=mounted>=0?vfs_find(name):NULL;
        if(!m || m->fs!=v) { usb_test_authorize(disk-4,0,0);return -1; }
        m->writable=1;
    }
#endif
    return mounted;
}
