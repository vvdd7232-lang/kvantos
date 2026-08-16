/* ============================================================
 *  KvantOS - драйвер жёсткого диска ATA (PIO, LBA28)
 *
 *  Работаем в режиме программного ввода-вывода: процессор сам
 *  перекладывает каждое слово через порт 0x1F0. Это медленно
 *  (около мегабайта в секунду), зато не требует ни DMA, ни
 *  настройки шины, ни прерываний - что для нашей задачи идеально:
 *  установщик копирует десятки килобайт, а не гигабайты.
 *
 *  Поддерживается LBA28: адресуется 2^28 секторов по 512 байт,
 *  то есть 128 ГиБ. Для диска, куда ставят KvantOS, с запасом.
 * ============================================================ */
#include "kernel.h"

/* Порты первого и второго каналов IDE */
#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

/* Смещения регистров относительно базового порта */
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

/* Биты регистра состояния */
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
    u16  io;             /* базовый порт данных   */
    u16  ctrl;           /* порт управления       */
    u8   slave;          /* 0 - master, 1 - slave */
    u8   present;        /* диск найден           */
    u32  sectors;        /* размер в секторах     */
    char model[41];      /* модель из IDENTIFY    */
} ata_drive_t;

static ata_drive_t drives[ATA_MAX_DRIVES];
static int drive_count = 0;
static int boot_drive  = -1;     /* выбранный для установки */

/* Задержка ~400 нс: четыре холостых чтения регистра состояния.
   Требуется по спецификации после смены выбранного диска. */
static void ata_delay(u16 ctrl) {
    for (int i = 0; i < 4; i++) inb(ctrl);
}

/* Ждём сброса BSY. Таймаут защищает от зависания на мёртвом порту:
   без него отсутствующий контроллер вешал бы загрузку намертво. */
static int ata_wait_busy(u16 io) {
    for (u32 i = 0; i < 400000; i++) {
        u8 st = inb(io + REG_STATUS);
        if (!(st & ST_BSY)) return 0;
    }
    return -1;
}

/* Ждём готовности данных (DRQ) либо ошибки */
static int ata_wait_drq(u16 io) {
    for (u32 i = 0; i < 400000; i++) {
        u8 st = inb(io + REG_STATUS);
        if (st & ST_ERR) return -1;
        if (st & ST_DF)  return -1;
        if (st & ST_DRQ) return 0;
    }
    return -1;
}

/* Строки в ответе IDENTIFY хранятся парами байтов наоборот */
static void fix_string(char *dst, const u16 *src, int words) {
    int n = 0;
    for (int i = 0; i < words; i++) {
        dst[n++] = (char)(src[i] >> 8);
        dst[n++] = (char)(src[i] & 0xFF);
    }
    dst[n] = 0;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
}

/* Опрос одного устройства командой IDENTIFY */
static void ata_identify(u16 io, u16 ctrl, u8 slave) {
    if (drive_count >= ATA_MAX_DRIVES) return;

    /* Плавающая шина: если статус 0xFF, на канале физически никого нет */
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
    if (st == 0) return;                       /* устройства нет */
    if (ata_wait_busy(io) < 0) return;

    /* Ненулевые LBA1/LBA2 означают ATAPI (привод CD) - нам не подходит */
    if (inb(io + REG_LBA1) != 0 || inb(io + REG_LBA2) != 0) return;
    if (ata_wait_drq(io) < 0) return;

    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io + REG_DATA);

    ata_drive_t *d = &drives[drive_count];
    d->io = io; d->ctrl = ctrl; d->slave = slave; d->present = 1;
    /* Слова 60-61 - число секторов в режиме LBA28 */
    d->sectors = ((u32)id[61] << 16) | id[60];
    fix_string(d->model, &id[27], 20);
    if (!d->model[0]) strcpy(d->model, "ATA диск");
    drive_count++;
}

void ata_init(void) {
    drive_count = 0;
    boot_drive  = -1;
    memset(drives, 0, sizeof(drives));

    /* Отключаем прерывания от контроллера: работаем строго опросом */
    outb(ATA_PRIMARY_CTRL, 0x02);
    outb(ATA_SECONDARY_CTRL, 0x02);

    ata_identify(ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   0);
    ata_identify(ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   1);
    ata_identify(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0);
    ata_identify(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1);

    /* Для установки берём первый диск разумного размера */
    for (int i = 0; i < drive_count; i++)
        if (drives[i].present && drives[i].sectors >= 2048) { boot_drive = i; break; }
}

int         ata_count(void)      { return drive_count; }
int         ata_boot_drive(void) { return boot_drive; }
int         ata_present(void)    { return boot_drive >= 0; }

const char *ata_model(int i) {
    if (i < 0 || i >= drive_count) return "нет";
    return drives[i].model;
}

u32 ata_sectors(int i) {
    if (i < 0 || i >= drive_count) return 0;
    return drives[i].sectors;
}

/* Размер диска в мегабайтах. Считаем в 32 битах: секторов не больше
   2^28, поэтому сдвиг на 11 (÷2048) не переполняется. */
u32 ata_size_mb(int i) {
    if (i < 0 || i >= drive_count) return 0;
    return drives[i].sectors >> 11;
}

/* Общая часть чтения и записи: выбрать диск и выставить адрес LBA28 */
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

/* Чтение count секторов начиная с lba. Возвращает 0 или -1. */
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

/* Запись count секторов. После записи обязателен сброс кэша диска,
   иначе при выключении питания данные останутся в буфере накопителя. */
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
        /* Пауза между словами не нужна, но выгружать надо ровно 256 слов */
        for (int i = 0; i < 256; i++) outw(d->io + REG_DATA, *p++);
    }

    outb(d->io + REG_COMMAND, CMD_FLUSH);
    ata_wait_busy(d->io);
    irq_restore(fl);
    return rc;
}
