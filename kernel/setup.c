/* ============================================================
 *  KvantOS - установка системы на жёсткий диск
 *
 *  Задача: сделать так, чтобы после установки компьютер грузился
 *  с винчестера сам, без флешки и привода - как обычная система.
 *
 *  Что для этого нужно записать на диск:
 *
 *    сектор 0          MBR: код запуска + таблица разделов
 *    секторы 1..2047   тело загрузчика GRUB (core.img)
 *    сектор 2048+      файловая система KvFS с приложениями
 *
 *  Ядро и приложения вложены ВНУТРЬ загрузчика (memdisk), поэтому
 *  отдельный раздел с файлами не нужен: GRUB читает всё из своего
 *  образа. Это тот же приём, что спас загрузку с дискеты.
 *
 *  Образ загрузчика приходит с установочного носителя как модуль
 *  Multiboot под именем hdboot.img и лежит в ramfs.
 * ============================================================ */
#include "kernel.h"

#define SECTOR_SIZE   512
#define KVFS_BASE     2048            /* совпадает с kernel/kvfs.c */
#define BOOT_IMAGE    "hdboot.img"

/* Ход установки виден снаружи: окно установщика рисует полосу */
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

/* Есть ли на носителе образ загрузчика? Без него установка
   бессмысленна: записать было бы нечего. */
int setup_available(void) {
    return ramfs_find(BOOT_IMAGE) != NULL;
}

/* ------------------------------------------------------------
 *  Таблица разделов
 *
 *  Раздел создаём один, на весь диск начиная с сектора 2048.
 *  Тип 0x83 (Linux) выбран намеренно: это честное «здесь чужая
 *  файловая система», и Windows не предложит его отформатировать.
 *  Флаг 0x80 - раздел активный, иначе BIOS откажется грузиться.
 * ------------------------------------------------------------ */
static void fill_partition(u8 *mbr, u32 total_sectors) {
    u8 *p = mbr + 0x1BE;                  /* первая из четырёх записей */
    u32 start = KVFS_BASE;
    u32 count = total_sectors - start;

    /* CHS давно никем не читается, но поле должно быть заполнено:
       ставим «максимум», как это делают современные установщики. */
    p[0]  = 0x80;                         /* активный */
    p[1]  = 0xFE; p[2] = 0xFF; p[3] = 0xFF;   /* CHS начала (заглушка) */
    p[4]  = 0x83;                         /* тип: своя ФС */
    p[5]  = 0xFE; p[6] = 0xFF; p[7] = 0xFF;   /* CHS конца */
    p[8]  = (u8)(start      ); p[9]  = (u8)(start >>  8);
    p[10] = (u8)(start >> 16); p[11] = (u8)(start >> 24);
    p[12] = (u8)(count      ); p[13] = (u8)(count >>  8);
    p[14] = (u8)(count >> 16); p[15] = (u8)(count >> 24);

    /* остальные три записи оставляем пустыми */
    memset(mbr + 0x1CE, 0, 16 * 3);

    mbr[0x1FE] = 0x55;                    /* подпись загрузочного сектора */
    mbr[0x1FF] = 0xAA;
}

/* ------------------------------------------------------------
 *  Установка
 *
 *  Возвращает 0 при успехе. Текст ошибки - в setup_last_result().
 * ------------------------------------------------------------ */
int setup_install(int keep_files) {
    setup_running = 1;
    setup_result[0] = 0;
    setup_percent = 0;

    if (!ata_present()) {
        strncpy(setup_result, "жёсткий диск не найден", sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    rfile_t *img = ramfs_find(BOOT_IMAGE);
    if (!img || img->size < SECTOR_SIZE) {
        strncpy(setup_result, "на носителе нет образа загрузчика", sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    int dev = ata_boot_drive();
    u32 total = ata_sectors(dev);
    u32 need  = (img->size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    if (need > KVFS_BASE - 1) {
        strncpy(setup_result, "загрузчик не помещается в первый мегабайт", sizeof(setup_result));
        setup_running = 0;
        return -1;
    }
    if (total < KVFS_BASE + 128) {
        strncpy(setup_result, "диск слишком мал (нужно от 2 МиБ)", sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    static u8 sec[SECTOR_SIZE];
    const u8 *src = (const u8 *)img->data;

    /* --- 1. Главная загрузочная запись ---
       Первые 446 байт - код из boot.img, дальше наша таблица
       разделов. Порядок важен: если сначала записать таблицу,
       а потом код, он её затрёт. */
    stage("Запись главной загрузочной записи", 10);
    memcpy(sec, src, 446);
    fill_partition(sec, total);
    if (ata_write(dev, 0, 1, sec) < 0) {
        strncpy(setup_result, "не удалось записать загрузочный сектор", sizeof(setup_result));
        setup_running = 0;
        return -1;
    }

    /* --- 2. Тело загрузчика ---
       Идёт со второго сектора подряд. boot.img знает, что искать
       продолжение именно там. */
    stage("Запись загрузчика", 20);
    u32 written = 0;
    for (u32 i = 1; i < need; i++) {
        u32 off = i * SECTOR_SIZE;
        u32 chunk = img->size - off;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        memset(sec, 0, SECTOR_SIZE);
        memcpy(sec, src + off, chunk);
        if (ata_write(dev, i, 1, sec) < 0) {
            strncpy(setup_result, "ошибка записи загрузчика на диск", sizeof(setup_result));
            setup_running = 0;
            return -1;
        }
        written++;
        /* полоса от 20 до 70 процентов */
        setup_percent = 20 + (int)((written * 50) / (need ? need : 1));
    }

    /* --- 3. Файловая система ---
       Если на диске уже есть KvFS и пользователь просил сохранить
       файлы, не трогаем её: установка поверх не должна стирать
       документы. Иначе создаём заново. */
    stage("Подготовка файловой системы", 75);
    int had_fs = (kvfs_mount() == 0);
    if (!had_fs || !keep_files) {
        int rc = kvfs_format();
        if (rc != 0) {
            ksnprintf(setup_result, sizeof(setup_result),
                      "не удалось создать файловую систему: %s", kvfs_error(rc));
            setup_running = 0;
            return -1;
        }
    }

    /* --- 4. Перенос приложений с носителя на диск ---
       Чтобы после установки программы были на месте. */
    stage("Копирование приложений", 85);
    rfile_t *tbl = ramfs_table();
    int copied = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!tbl[i].used) continue;
        if (!strcmp(tbl[i].name, BOOT_IMAGE)) continue;   /* сам загрузчик не нужен */

        u32 l = (u32)strlen(tbl[i].name);
        int is_app = (l > 5 && !strcmp(tbl[i].name + l - 5, ".kapp"));

        /* существующий файл не перетираем, если просили сохранить */
        if (keep_files && kvfs_exists(tbl[i].name)) continue;

        if (kvfs_write(tbl[i].name, tbl[i].data, tbl[i].size, is_app) == 0) copied++;
    }

    stage("Завершение", 100);
    ksnprintf(setup_result, sizeof(setup_result),
              "Готово. Скопировано файлов: %d. Можно перезагружаться.", copied);
    setup_running = 0;
    return 0;
}
