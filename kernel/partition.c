/* ============================================================
 *  KvantOS - MBR/GPT partition tables and volume autodetection
 *
 *  Reads sector 0 of every ATA/USB disk, walks the four primary entries
 *  and the extended chain, then asks each filesystem driver whether it
 *  recognises the partition. Whatever is recognised gets mounted under
 *  /mnt/hdaN.
 *
 *  A disk with no partition table at all (a "superfloppy": some USB
 *  sticks and our own kvantos-disk.img) is probed as a whole too.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"

#define MBR_SIG_OFF     0x1FE
#define PART_TABLE_OFF  0x1BE

typedef struct {
    u8  boot;
    u8  chs_start[3];
    u8  type;
    u8  chs_end[3];
    u32 lba_start;
    u32 sectors;
} __attribute__((packed)) mbr_part_t;

/* Partition types worth looking at. The type byte is only a hint - the
   real check is the driver probe - but it lets us skip obvious junk. */
static int type_interesting(u8 t) {
    switch (t) {
        case 0x01: case 0x04: case 0x06:            /* FAT12/16      */
        case 0x0B: case 0x0C:                       /* FAT32         */
        case 0x0E:                                  /* FAT16 LBA     */
        case 0x07:                                  /* NTFS/exFAT    */
        case 0x83:                                  /* Linux (KvFS)  */
        case 0x0F: case 0x05:                       /* extended      */
            return 1;
        default:
            return t != 0;      /* try anything non-empty anyway */
    }
}

static int is_extended(u8 t) { return t == 0x05 || t == 0x0F || t == 0x85; }

/* Try every driver we have on one partition. */
static int try_mount(const char *name, int disk, u32 lba, u32 count) {
    u32 sectors = blk_sectors(disk);
    if (!count || lba >= sectors || count > sectors - lba) return 0;
    char label[VFS_LABEL_MAX];
    fs_kind_t kind = FS_NONE;

    label[0] = 0;
    if (fat_probe(disk, lba, count, label, &kind))
        return fat_mount(name, disk, lba, count) >= 0;

    label[0] = 0;
    if (ntfs_probe(disk, lba, count, label))
        return ntfs_mount(name, disk, lba, count) >= 0;

    return 0;
}

static void volume_name(char *nm, int disk, int seq) {
    if (blk_is_usb(disk)) {
        if (seq) ksnprintf(nm,16,"usb%dp%d",disk-4,seq);
        else ksnprintf(nm,16,"usb%d",disk-4);
    } else {
        if (seq) ksnprintf(nm,16,"hd%c%d",'a'+disk,seq);
        else ksnprintf(nm,16,"hd%c",'a'+disk);
    }
}
/* GPT 1.0, 512-byte logical sectors, read-only mounts. The block layer is
 * 32-bit LBA, so validate every 64-bit value before narrowing. Header and
 * entire entry-array CRCs are checked BEFORE mounting anything. */
static const char *scan_status;
const char *vfs_scan_status(void) { return scan_status?scan_status:""; }
typedef struct { u32 first,last,entries,count,size; } gpt_table_t;
static u32 gpt_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}
static u64 gpt_u64(const u8 *p) { return gpt_u32(p) | ((u64)gpt_u32(p+4)<<32); }
static u32 gpt_crc(u32 crc, const u8 *p, u32 size) {
    while (size--) {
        crc ^= *p++;
        for (int bit=0;bit<8;bit++) crc=(crc>>1)^((crc&1)?0xEDB88320u:0);
    }
    return crc;
}
static int gpt_table(int disk, u32 at, gpt_table_t *t) {
    static u8 h[512], sector[512];
    u32 total=blk_sectors(disk);
    if (total<68 || !disk_read(disk,at,1,h) || memcmp(h,"EFI PART",8)) return 0;
    u32 size=gpt_u32(h+12), expected=gpt_u32(h+16);
    if (gpt_u32(h+8)!=0x10000 || size<92 || size>512 || gpt_u32(h+20)) return 0;
    memset(h+16,0,4);
    if ((gpt_crc(0xFFFFFFFFu,h,size)^0xFFFFFFFFu)!=expected) return 0;
    if (gpt_u64(h+24)!=at || gpt_u64(h+32)!=(at==1?total-1:1)) return 0;
    u64 first=gpt_u64(h+40), last=gpt_u64(h+48), entries=gpt_u64(h+72);
    u32 count=gpt_u32(h+80), stride=gpt_u32(h+84);
    if (first<2 || first>last || last>=total-1 || entries<2 || entries>=total-1) return 0;
    /* Bounded scan: Windows uses 128 x 128 B. Larger standard strides are
     * accepted up to 4096 B, 1024 slots and 128 KiB total metadata. */
    if (!count || count>1024 || stride<128 || stride>4096 || (stride&(stride-1))) return 0;
    u32 bytes=count*stride;
    if (bytes>128*1024) return 0;
    u32 sectors=(bytes+511)/512;
    if (first<2+(u64)sectors || last+sectors+2>total) return 0;
    if (sectors>total-1-entries) return 0;
    if (at==1 ? entries+sectors>first : entries<=last) return 0;
    u32 crc=0xFFFFFFFFu;
    for (u32 done=0;done<bytes;) {
        if (!disk_read(disk,(u32)entries+done/512,1,sector)) return 0;
        u32 n=bytes-done; if (n>512) n=512;
        crc=gpt_crc(crc,sector,n); done+=n;
    }
    if ((crc^0xFFFFFFFFu)!=gpt_u32(h+88)) return 0;
    t->first=(u32)first; t->last=(u32)last; t->entries=(u32)entries;
    t->count=count; t->size=stride;
    return 1;
}
static int scan_gpt(int disk) {
    scan_status=T("GPT: unreadable/invalid header or partition-table CRC", "GPT: ошибка чтения или CRC заголовка/таблицы разделов");
    gpt_table_t t;
    u32 total=blk_sectors(disk);
    if (total<68 || (!gpt_table(disk,1,&t) && !gpt_table(disk,total-1,&t))) return 0;
    scan_status=T("GPT valid; no supported FAT volume found", "GPT прочитана, но поддерживаемый FAT-том не найден");
    static u8 sector[512];
    u32 loaded=0xFFFFFFFFu;
    int mounted=0;
    for (u32 i=0;i<t.count;i++) {
        u32 off=i*t.size, lba=t.entries+off/512;
        if (loaded!=lba) {
            if (!disk_read(disk,lba,1,sector)) return mounted;
            loaded=lba;
        }
        const u8 *p=sector+off%512;
        unsigned type=0;
        for (int n=0;n<16;n++) type|=p[n];
        if (!type) continue; /* unused slot, not an MSR/ESP/data partition */
        u64 start=gpt_u64(p+32), end=gpt_u64(p+40);
        if (start<t.first || start>end || end>t.last) continue;
        char name[16]; volume_name(name,disk,(int)i+1); /* preserve GPT slot number */
        if (try_mount(name,disk,(u32)start,(u32)(end-start+1))) {
            vfs_volume_t *v=vfs_find(name);
            if (v) v->writable=0; /* GPT is read-only, including on ATA */
            mounted++;
            scan_status=T("GPT volume mounted read-only", "GPT-том подключён только для чтения");
        }
    }
    return mounted;
}
static int scan_disk(int disk, int *seq) {
    static u8 sec[512];      /* BSS, not stack: see the FAT driver */
    int mounted = 0;

    if (!disk_read(disk, 0, 1, sec)) return 0;

    int has_table = (sec[MBR_SIG_OFF] == 0x55 && sec[MBR_SIG_OFF + 1] == 0xAA);

    if (has_table) {
        mbr_part_t parts[4];
        memcpy(parts, sec + PART_TABLE_OFF, sizeof(parts));
        /* A protective/hybrid MBR declares GPT. Never reinterpret broken GPT
         * metadata as raw FAT or mount the hybrid aliases a second time. */
        for (int i=0;i<4;i++)
            if (parts[i].type==0xEE && parts[i].lba_start==1 && parts[i].sectors) return scan_gpt(disk);


        for (int i = 0; i < 4; i++) {
            if (!parts[i].sectors || !type_interesting(parts[i].type)) continue;

            if (is_extended(parts[i].type)) {
                /* Walk the logical-partition chain. Each link holds one
                   partition plus a pointer to the next link, both
                   relative to the start of the extended partition. */
                u32 base = parts[i].lba_start, next = parts[i].lba_start;
                for (int guard = 0; guard < 16 && next; guard++) {
                    static u8 ebr[512];
                    if (!disk_read(disk, next, 1, ebr)) break;
                    if (ebr[MBR_SIG_OFF] != 0x55 || ebr[MBR_SIG_OFF + 1] != 0xAA) break;

                    mbr_part_t lp[2];
                    memcpy(lp, ebr + PART_TABLE_OFF, sizeof(lp));

                    if (lp[0].sectors) {
                        char nm[16];
                        volume_name(nm, disk, ++(*seq));
                        if (try_mount(nm, disk, next + lp[0].lba_start, lp[0].sectors))
                            mounted++;
                    }
                    next = lp[1].sectors ? base + lp[1].lba_start : 0;
                }
                continue;
            }

            char nm[16];
            volume_name(nm, disk, ++(*seq));
            if (try_mount(nm, disk, parts[i].lba_start, parts[i].sectors))
                mounted++;
        }
    }

    /* No table, or a table nobody claimed: probe the raw device. */
    if (!mounted) {
        char nm[16];
        volume_name(nm, disk, 0);
        if (try_mount(nm, disk, 0, blk_sectors(disk))) mounted++;
    }

    return mounted;
}

int vfs_autoscan(void) {
    scan_status=NULL;
    int total = 0;
    usb_rescan();

    /* RAM disk and our own KvFS keep their historical names. */
    ramfs_vfs_register();
    kvfs_vfs_register();

    for (int d = 0; d < blk_count(); d++) {
        if (!blk_sectors(d)) continue;
        int seq = 0;
        total += scan_disk(d, &seq);
    }
    return total;
}
