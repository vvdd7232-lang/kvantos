/* Stable IDs: ATA 0..3, USB 4..11, AHCI 12..19. Gaps are absent devices.
 * KvFS/installer remain ATA-only. USB/AHCI write grants remain transport-enforced. */
#include "kernel.h"
int blk_is_usb(int disk) { return disk>=BLK_USB_BASE && disk<BLK_AHCI_BASE; }
int blk_is_ahci(int disk) { return disk>=BLK_AHCI_BASE && disk<BLK_AHCI_BASE+8; }
int blk_count(void) {
    if(ahci_count()) return BLK_AHCI_BASE+ahci_count();
    return usb_count()?BLK_USB_BASE+usb_count():ata_count();
}
u32 blk_sectors(int disk) {
    if(blk_is_ahci(disk)) return ahci_sectors(disk-BLK_AHCI_BASE);
    if(blk_is_usb(disk)) return usb_sectors(disk-BLK_USB_BASE);
    return disk>=0 && disk<BLK_USB_BASE?ata_sectors(disk):0;
}
int blk_read(int disk,u32 lba,u8 count,void *buf) {
    u32 size=blk_sectors(disk);
    if(!buf || !count || lba>=size || count>size-lba) return -1;
    if(blk_is_ahci(disk)) return ahci_read(disk-BLK_AHCI_BASE,lba,count,buf);
    return blk_is_usb(disk)?usb_read(disk-BLK_USB_BASE,lba,count,buf):ata_read(disk,lba,count,buf);
}
int blk_write(int disk,u32 lba,u8 count,const void *buf) {
    u32 size=blk_sectors(disk);
    if(!buf || !count || lba>=size || count>size-lba) return -1;
    if(blk_is_ahci(disk)) return ahci_write(disk-BLK_AHCI_BASE,lba,count,buf);
    return blk_is_usb(disk)?usb_write(disk-BLK_USB_BASE,lba,count,buf):ata_write(disk,lba,count,buf);
}
int blk_sync(int disk) {
    if(!blk_sectors(disk)) return -1;
    if(blk_is_ahci(disk)) return ahci_sync(disk-BLK_AHCI_BASE);
    return blk_is_usb(disk)?usb_sync(disk-BLK_USB_BASE):ata_sync(disk);
}

u32 blk_generation(int disk) {
    if(blk_is_ahci(disk)) return ahci_generation(disk-BLK_AHCI_BASE);
    return blk_is_usb(disk)?usb_generation(disk-BLK_USB_BASE):0;
}
int blk_fat_authorize(int disk,u32 start,u32 count,u32 generation) {
    if(blk_is_ahci(disk)) return ahci_fat_authorize(disk-BLK_AHCI_BASE,start,count,generation);
    if(blk_is_usb(disk)) return usb_fat_authorize(disk-BLK_USB_BASE,start,count,generation);
    u32 size=blk_sectors(disk);
    return count && start<size && count<=size-start?0:-1;
}
void blk_fat_revoke(int disk) {
    if(blk_is_ahci(disk)) ahci_fat_revoke(disk-BLK_AHCI_BASE);
    else if(blk_is_usb(disk)) usb_fat_revoke(disk-BLK_USB_BASE);
}
