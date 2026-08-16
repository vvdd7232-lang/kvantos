/* ============================================================
 *  KvantOS - installing the system onto a hard disk
 *
 *  The goal: after installation the computer must boot from the hard
 *  disk on its own, with no USB stick and no optical drive - like a
 *  normal system.
 *
 *  What has to be written to the disk for that:
 *
 *    sector 0          MBR: boot code + partition table
 *    sectors 1..2047   the body of the GRUB bootloader (core.img)
 *    sector 2048+      the KvFS filesystem holding the applications
 *
 *  The kernel and the applications are embedded INSIDE the bootloader
 *  (memdisk), so a separate partition for files is unnecessary: GRUB
 *  reads everything from its own image. This is the same trick that
 *  rescued booting from a floppy.
 *
 *  The bootloader image arrives from the install media as a Multiboot
 *  module named hdboot.img and lives in ramfs.
 * ============================================================ */
#include "kernel.h"

#define SECTOR_SIZE   512
#define KVFS_BASE     2048            /* matches kernel/kvfs.c */
#define BOOT_IMAGE    "hdboot.img"

/* Progress is visible from outside: the installer window draws a bar */
static int   setup_percent = 0;
static int   setup_running = 0;
static char  setup_stage[64]  = "";
static char  setup_result[96] = "";

int         setup_progress(void) { return setup_percent; }
int         setup_busy(void)     { return setup_running; }
const char *setup_stage_text(void) { return setup_stage; }
const char *setup_last_result(void) { return setup_result; }

static void stage(const char *s, int pct) {
    strncpy(setup_stage, s, sizeof(setup_stage));
    setup_stage[sizeof(setup_stage) - 1] = 0;
    setup_percent = pct;
}

/* Does the media carry a bootloader image? Without one installing is
   pointless: there would be nothing to write. */
int setup_available(void) {
    return ramfs_find(BOOT_IMAGE) != NULL;
}

/* ------------------------------------------------------------
 *  Partition table
 *
 *  A single partition is created, spanning the disk from sector 2048.
 *  Type 0x83 (Linux) is deliberate: it honestly says "a foreign
 *  filesystem lives here", and Windows will not offer to format it.
 *  Flag 0x80 marks the partition active, otherwise the BIOS refuses to
 *  boot from it.
 * ------------------------------------------------------------ */
static void fill_partition(u8 *mbr, u32 total_sectors) {
    u8 *p = mbr + 0x1BE;                  /* the first of four entries */
    u32 start = KVFS_BASE;
    u32 count = total_sectors - start;

    /* Nobody has read CHS for decades, but the field must be filled in:
       we store the "maximum", exactly as modern installers do. */
    p[0]  = 0x80;                         /* active */
    p[1]  = 0xFE; p[2] = 0xFF; p[3] = 0xFF;   /* starting CHS (a stub) */
    p[4]  = 0x83;                         /* type: our own filesystem */
    p[5]  = 0xFE; p[6] = 0xFF; p[7] = 0xFF;   /* ending CHS */
    p[8]  = (u8)(start      ); p[9]  = (u8)(start >>  8);
    p[10] = (u8)(start >> 16); p[11] = (u8)(start >> 24);
    p[12] = (u8)(count      ); p[13] = (u8)(count >>  8);
    p[14] = (u8)(count >> 16); p[15] = (u8)(count >> 24);

    /* the other three entries are left empty */
    memset(mbr + 0x1CE, 0, 16 * 3);

    mbr[0x1FE] = 0x55;                    /* boot sector signature */
    mbr[0x1FF] = 0xAA;
}

/* ------------------------------------------------------------
 *  Installation
 *
 *  Returns 0 on success. The error text goes to setup_last_result().
 * ------------------------------------------------------------ */
int setup_install(int keep_files) {
    setup_running = 1;
    setup_result[0] = 0;
    setup_percent = 0;

    if (!ata_present()) {
        strncpy(setup_result, T("no hard disk found", "жёсткий диск не найден"), sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    rfile_t *img = ramfs_find(BOOT_IMAGE);
    if (!img || img->size < SECTOR_SIZE) {
        strncpy(setup_result, T("the media carries no bootloader image", "на носителе нет образа загрузчика"), sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    int dev = ata_boot_drive();
    u32 total = ata_sectors(dev);
    u32 need  = (img->size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    if (need > KVFS_BASE - 1) {
        strncpy(setup_result, T("the bootloader does not fit in the first megabyte", "загрузчик не помещается в первый мегабайт"), sizeof(setup_result));
        setup_running = 0;
        return -1;
    }
    if (total < KVFS_BASE + 128) {
        strncpy(setup_result, T("disk too small (2 MiB minimum)", "диск слишком мал (нужно от 2 МиБ)"), sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    static u8 sec[SECTOR_SIZE];
    const u8 *src = (const u8 *)img->data;

    /* --- 1. Master boot record ---
       The first 446 bytes are the code from boot.img, our partition
       table follows. Order matters: writing the table first and the
       code afterwards would overwrite the table. */
    stage(T("Writing the master boot record", "Запись главной загрузочной записи"), 10);
    memcpy(sec, src, 446);
    fill_partition(sec, total);
    if (ata_write(dev, 0, 1, sec) < 0) {
        strncpy(setup_result, T("could not write the boot sector", "не удалось записать загрузочный сектор"), sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    /* --- 2. The bootloader body ---
       Laid down contiguously from the second sector. boot.img knows
       to look for its continuation exactly there. */
    stage(T("Writing the bootloader", "Запись загрузчика"), 20);
    u32 written = 0;
    for (u32 i = 1; i < need; i++) {
        u32 off = i * SECTOR_SIZE;
        u32 chunk = img->size - off;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        memset(sec, 0, SECTOR_SIZE);
        memcpy(sec, src + off, chunk);
        if (ata_write(dev, i, 1, sec) < 0) {
            strncpy(setup_result, T("error writing the bootloader to disk", "ошибка записи загрузчика на диск"), sizeof(setup_result));
            setup_running = 0;
            return -1;
        }
        written++;
        /* the bar runs from 20 to 70 per cent */
        setup_percent = 20 + (int)((written * 50) / (need ? need : 1));
    }

    /* --- 3. The filesystem ---
       If the disk already holds KvFS and the user asked to keep the
       files, leave it alone: installing over an existing system must
       not erase documents. Otherwise create it from scratch. */
    stage(T("Preparing the filesystem", "Подготовка файловой системы"), 75);
    int had_fs = (kvfs_mount() == 0);
    if (!had_fs || !keep_files) {
        int rc = kvfs_format();
        if (rc != 0) {
            ksnprintf(setup_result, sizeof(setup_result),
                      T("could not create the filesystem: %s", "не удалось создать файловую систему: %s"), kvfs_error(rc));
            setup_running = 0;
            return -1;
        }
    }

    /* --- 4. Moving applications from the media onto the disk ---
       So that the programs are in place after the installation. */
    stage(T("Copying applications", "Копирование приложений"), 85);
    rfile_t *tbl = ramfs_table();
    int copied = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!tbl[i].used) continue;
        if (!strcmp(tbl[i].name, BOOT_IMAGE)) continue;   /* the bootloader itself is not needed */

        u32 l = (u32)strlen(tbl[i].name);
        int is_app = (l > 5 && !strcmp(tbl[i].name + l - 5, ".kapp"));

        /* an existing file is not overwritten when keeping was requested */
        if (keep_files && kvfs_exists(tbl[i].name)) continue;

        if (kvfs_write(tbl[i].name, tbl[i].data, tbl[i].size, is_app) == 0) copied++;
    }

    stage(T("Finishing up", "Завершение"), 100);
    ksnprintf(setup_result, sizeof(setup_result),
              T("Done. Files copied: %d. You can reboot now.", "Готово. Скопировано файлов: %d. Можно перезагружаться."), copied);
    setup_running = 0;
    return 0;
}
