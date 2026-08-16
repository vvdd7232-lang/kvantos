/* ============================================================
 *  KvantOS - MBR partition table and volume autodetection
 *
 *  Reads sector 0 of every ATA disk, walks the four primary entries
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

static int scan_disk(int disk, int *seq) {
    static u8 sec[512];      /* BSS, not stack: see the FAT driver */
    int mounted = 0;

    if (!disk_read(disk, 0, 1, sec)) return 0;

    int has_table = (sec[MBR_SIG_OFF] == 0x55 && sec[MBR_SIG_OFF + 1] == 0xAA);

    if (has_table) {
        mbr_part_t parts[4];
        memcpy(parts, sec + PART_TABLE_OFF, sizeof(parts));

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
                        ksnprintf(nm, sizeof(nm), "hd%c%d", 'a' + disk, ++(*seq));
                        if (try_mount(nm, disk, next + lp[0].lba_start, lp[0].sectors))
                            mounted++;
                    }
                    next = lp[1].sectors ? base + lp[1].lba_start : 0;
                }
                continue;
            }

            char nm[16];
            ksnprintf(nm, sizeof(nm), "hd%c%d", 'a' + disk, ++(*seq));
            if (try_mount(nm, disk, parts[i].lba_start, parts[i].sectors))
                mounted++;
        }
    }

    /* No table, or a table nobody claimed: probe the raw device. */
    if (!mounted) {
        char nm[16];
        ksnprintf(nm, sizeof(nm), "hd%c", 'a' + disk);
        if (try_mount(nm, disk, 0, ata_sectors(disk))) mounted++;
    }

    return mounted;
}

int vfs_autoscan(void) {
    int total = 0;

    /* RAM disk and our own KvFS keep their historical names. */
    ramfs_vfs_register();
    kvfs_vfs_register();

    for (int d = 0; d < ata_count(); d++) {
        if (!ata_sectors(d)) continue;
        int seq = 0;
        total += scan_disk(d, &seq);
    }
    return total;
}
