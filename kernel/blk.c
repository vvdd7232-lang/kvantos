/* Stable IDs: ATA 0..3; USB 4.. . KvFS/installer stay ATA-only. */
#include "kernel.h"
int blk_is_usb(int disk) { return disk >= 4; }
int blk_count(void) { return usb_count() ? 4 + usb_count() : ata_count(); }
u32 blk_sectors(int disk) {
    if (disk < 0) return 0;
    return blk_is_usb(disk) ? usb_sectors(disk - 4) : ata_sectors(disk);
}
int blk_read(int disk, u32 lba, u8 count, void *buf) {
    u32 size = blk_sectors(disk);
    if (!count || lba >= size || count > size - lba) return -1;
    return blk_is_usb(disk) ? usb_read(disk - 4, lba, count, buf) : ata_read(disk, lba, count, buf);
}
int blk_write(int disk, u32 lba, u8 count, const void *buf) {
    u32 size = blk_sectors(disk);
    if (blk_is_usb(disk) || !count || lba >= size || count > size - lba) return -1;
    return ata_write(disk, lba, count, buf);
}
