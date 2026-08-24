/* ============================================================
 *  KvantOS - unified block devices
 *
 *  ATA disks come first (indices 0 .. ata_count-1); USB mass-storage
 *  drives follow. The partition scanner and the filesystems talk to
 *  this single layer, so a real USB flash drive is mounted exactly like
 *  a hard disk. USB disks are read-only.
 * ============================================================ */
#include "kernel.h"

static int ata_n = 0;

void blk_init(void) {
    ata_n = ata_count();   /* ATA was already discovered earlier in boot */
    usb_init();
}

int blk_count(void) {
    return ata_n + usb_disk_count();
}

u32 blk_sectors(int i) {
    if (i < ata_n) return ata_sectors(i);
    return usb_sectors(i - ata_n);
}

int blk_read(int i, u32 lba, u8 count, void *buf) {
    if (i < ata_n) return ata_read(i, lba, count, buf);
    return usb_read(i - ata_n, lba, count, buf);
}

int blk_write(int i, u32 lba, u8 count, const void *buf) {
    if (i < ata_n) return ata_write(i, lba, count, buf);
    return -1;     /* USB is read-only */
}

const char *blk_model(int i) {
    if (i < ata_n) return ata_model(i);
    return usb_model(i - ata_n);
}
