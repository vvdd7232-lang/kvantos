/* ============================================================
 *  KvantOS - ATA hard disk driver (PIO, LBA28)
 *
 *  We work in programmed I/O mode: the CPU itself moves every word
 *  through port 0x1F0. That is slow (about a megabyte per second)
 *  but needs no DMA, no bus configuration and no interrupts - which
 *  suits our task perfectly: the installer copies tens of kilobytes,
 *  not gigabytes.
 *
 *  LBA28 is supported: 2^28 sectors of 512 bytes are addressable,
 *  that is 128 GiB. Plenty for a disk that receives KvantOS.
 * ============================================================ */
#include "kernel.h"

/* Ports of the primary and secondary IDE channels */
#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

/* Register offsets relative to the base port */
#define REG_DATA       0
#define REG_ERROR      1
#define REG_FEATURES   1
#define REG_SECCOUNT   2
#define REG_LBA0       3
#define REG_LBA1       4
#define REG_LBA2       5
#define REG_DRIVE      6
#define REG_STATUS     7
#define REG_COMMAND    7

/* Status register bits */
#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_SRV  0x10
#define ST_DF   0x20
#define ST_RDY  0x40
#define ST_BSY  0x80

#define CMD_READ_PIO   0x20
#define CMD_WRITE_PIO  0x30
#define CMD_FLUSH      0xE7
#define CMD_IDENTIFY   0xEC

#define ATA_MAX_DRIVES 4

typedef struct {
    u16  io;             /* base data port        */
    u16  ctrl;           /* control port          */
    u8   slave;          /* 0 - master, 1 - slave */
    u8   present;        /* disk found            */
    u32  sectors;        /* size in sectors       */
    char model[41];      /* model from IDENTIFY   */
} ata_drive_t;

static ata_drive_t drives[ATA_MAX_DRIVES];
static int drive_count = 0;
static int boot_drive  = -1;     /* chosen for installation */

/* A ~400 ns delay: four dummy reads of the status register.
   The specification requires it after switching the selected disk. */
static void ata_delay(u16 ctrl) {
    for (int i = 0; i < 4; i++) inb(ctrl);
}

/* Wait for BSY to clear. The timeout guards against hanging on a dead
   port: without it a missing controller would freeze the boot for good. */
static int ata_wait_busy(u16 io) {
    for (u32 i = 0; i < 400000; i++) {
        u8 st = inb(io + REG_STATUS);
        if (!(st & ST_BSY)) return 0;
    }
    return -1;
}

/* Wait for data readiness (DRQ) or an error */
static int ata_wait_drq(u16 io) {
    for (u32 i = 0; i < 400000; i++) {
        u8 st = inb(io + REG_STATUS);
        if (st & ST_ERR) return -1;
        if (st & ST_DF)  return -1;
        if (st & ST_DRQ) return 0;
    }
    return -1;
}

/* Strings in the IDENTIFY reply are stored as byte-swapped pairs */
static void fix_string(char *dst, const u16 *src, int words) {
    int n = 0;
    for (int i = 0; i < words; i++) {
        dst[n++] = (char)(src[i] >> 8);
        dst[n++] = (char)(src[i] & 0xFF);
    }
    dst[n] = 0;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
}

/* Probe a single device with the IDENTIFY command */
static void ata_identify(u16 io, u16 ctrl, u8 slave) {
    if (drive_count >= ATA_MAX_DRIVES) return;

    /* Floating bus: a status of 0xFF means nobody is physically on the channel */
    if (inb(io + REG_STATUS) == 0xFF) return;

    outb(io + REG_DRIVE, (u8)(0xA0 | (slave << 4)));
    ata_delay(ctrl);
    outb(io + REG_SECCOUNT, 0);
    outb(io + REG_LBA0, 0);
    outb(io + REG_LBA1, 0);
    outb(io + REG_LBA2, 0);
    outb(io + REG_COMMAND, CMD_IDENTIFY);
    ata_delay(ctrl);

    u8 st = inb(io + REG_STATUS);
    if (st == 0) return;                       /* no device */
    if (ata_wait_busy(io) < 0) return;

    /* Non-zero LBA1/LBA2 mean ATAPI (a CD drive) - not what we want */
    if (inb(io + REG_LBA1) != 0 || inb(io + REG_LBA2) != 0) return;
    if (ata_wait_drq(io) < 0) return;

    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io + REG_DATA);

    ata_drive_t *d = &drives[drive_count];
    d->io = io; d->ctrl = ctrl; d->slave = slave; d->present = 1;
    /* Words 60-61 hold the sector count in LBA28 mode */
    d->sectors = ((u32)id[61] << 16) | id[60];
    fix_string(d->model, &id[27], 20);
    if (!d->model[0]) strcpy(d->model, T("ATA disk", "ATA диск"));
    drive_count++;
}

void ata_init(void) {
    drive_count = 0;
    boot_drive  = -1;
    memset(drives, 0, sizeof(drives));

    /* Disable controller interrupts: we work strictly by polling */
    outb(ATA_PRIMARY_CTRL, 0x02);
    outb(ATA_SECONDARY_CTRL, 0x02);

    ata_identify(ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   0);
    ata_identify(ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   1);
    ata_identify(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0);
    ata_identify(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1);

    /* For installation we take the first disk of a sensible size */
    for (int i = 0; i < drive_count; i++)
        if (drives[i].present && drives[i].sectors >= 2048) { boot_drive = i; break; }
}

int         ata_count(void)      { return drive_count; }
int         ata_boot_drive(void) { return boot_drive; }
int         ata_present(void)    { return boot_drive >= 0; }

const char *ata_model(int i) {
    if (i < 0 || i >= drive_count) return T("no", "нет");
    return drives[i].model;
}

u32 ata_sectors(int i) {
    if (i < 0 || i >= drive_count) return 0;
    return drives[i].sectors;
}

/* Disk size in megabytes. Computed in 32 bits: there are at most 2^28
   sectors, so shifting by 11 (dividing by 2048) cannot overflow. */
u32 ata_size_mb(int i) {
    if (i < 0 || i >= drive_count) return 0;
    return drives[i].sectors >> 11;
}

/* Shared by read and write: select the disk and set the LBA28 address */
static int ata_setup(ata_drive_t *d, u32 lba, u8 count) {
    if (ata_wait_busy(d->io) < 0) return -1;
    outb(d->io + REG_DRIVE, (u8)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
    ata_delay(d->ctrl);
    outb(d->io + REG_FEATURES, 0);
    outb(d->io + REG_SECCOUNT, count);
    outb(d->io + REG_LBA0, (u8)(lba & 0xFF));
    outb(d->io + REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(d->io + REG_LBA2, (u8)((lba >> 16) & 0xFF));
    return 0;
}

/* Read count sectors starting at lba. Returns 0 or -1. */
int ata_read(int idx, u32 lba, u8 count, void *buf) {
    if (idx < 0 || idx >= drive_count || !drives[idx].present) return -1;
    if (!count) return 0;
    ata_drive_t *d = &drives[idx];
    if (lba + count > d->sectors) return -1;

    u32 fl = irq_save();
    int rc = 0;
    if (ata_setup(d, lba, count) < 0) { irq_restore(fl); return -1; }
    outb(d->io + REG_COMMAND, CMD_READ_PIO);

    u16 *p = (u16 *)buf;
    for (u8 s = 0; s < count; s++) {
        if (ata_wait_busy(d->io) < 0 || ata_wait_drq(d->io) < 0) { rc = -1; break; }
        for (int i = 0; i < 256; i++) *p++ = inw(d->io + REG_DATA);
    }
    irq_restore(fl);
    return rc;
}

/* Write count sectors. Flushing the disk cache afterwards is mandatory,
   otherwise a power-off would leave the data in the drive's buffer. */
int ata_write(int idx, u32 lba, u8 count, const void *buf) {
    if (idx < 0 || idx >= drive_count || !drives[idx].present) return -1;
    if (!count) return 0;
    ata_drive_t *d = &drives[idx];
    if (lba + count > d->sectors) return -1;

    u32 fl = irq_save();
    int rc = 0;
    if (ata_setup(d, lba, count) < 0) { irq_restore(fl); return -1; }
    outb(d->io + REG_COMMAND, CMD_WRITE_PIO);

    const u16 *p = (const u16 *)buf;
    for (u8 s = 0; s < count; s++) {
        if (ata_wait_busy(d->io) < 0 || ata_wait_drq(d->io) < 0) { rc = -1; break; }
        /* No pause between words is needed, but exactly 256 words must be moved */
        for (int i = 0; i < 256; i++) outw(d->io + REG_DATA, *p++);
    }

    outb(d->io + REG_COMMAND, CMD_FLUSH);
    ata_wait_busy(d->io);
    irq_restore(fl);
    return rc;
}
