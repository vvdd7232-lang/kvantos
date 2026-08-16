/* ============================================================
 *  KvantOS - KvFS, простая файловая система на диске
 *
 *  Устроена нарочно примитивно, чтобы её можно было прочитать
 *  целиком за один присест и понять без документации:
 *
 *    сектор 0            - суперблок (подпись, счётчики)
 *    секторы 1..8        - каталог: 64 записи по 64 байта
 *    секторы 9..         - область данных, выдаётся кусками
 *
 *  Файл занимает непрерывную цепочку секторов. Фрагментации нет:
 *  при удалении дыра остаётся, а новый файл ищется по принципу
 *  «первый подходящий промежуток». Для системы, где файлы - это
 *  десяток приложений и заметок, этого более чем достаточно.
 *
 *  Все числа - little-endian, как их кладёт сам процессор.
 * ============================================================ */
#include "kernel.h"

#define KVFS_MAGIC     0x5346564Bu     /* "KVFS" */
#define KVFS_VERSION   2
#define SECTOR_SIZE    512

/* Первый мегабайт диска НЕ наш: там живёт загрузчик.
   Сектор 0 - MBR, секторы 1..2047 - тело GRUB (core.img).
   Ровно так же поступают обычные системы, начиная первый раздел
   с отметки 1 МиБ. Всё, что ниже, отсчитывается от этой базы. */
#define KVFS_BASE      2048

#define DIR_SECTOR     (KVFS_BASE + 1)
#define DIR_SECTORS    8
#define DATA_START     (DIR_SECTOR + DIR_SECTORS)
#define KVFS_MAX_FILES 64

/* Запись каталога - ровно 64 байта, восемь штук на сектор */
typedef struct {
    char name[40];      /* имя, дополненное нулями */
    u32  start;         /* первый сектор данных    */
    u32  sectors;       /* сколько секторов занято */
    u32  size;          /* фактический размер, байт */
    u32  flags;         /* 1 - запись занята, 2 - исполняемый файл */
    u32  reserved[2];   /* до ровных 64 байт: 8 записей на сектор */
} kvfs_dirent_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 total_sectors;
    u32 data_start;
    u32 file_count;
    u32 reserved[3];
} kvfs_super_t;

/* Проверка на этапе сборки: запись каталога обязана быть ровно 64 байта,
   иначе образы, собранные sdk/mkdisk.py, читались бы со сдвигом. */
_Static_assert(sizeof(kvfs_dirent_t) == 64, "запись каталога KvFS должна быть 64 байта");

static kvfs_super_t   super;
static kvfs_dirent_t  dir[KVFS_MAX_FILES];
static int mounted = 0;
static int dev     = -1;         /* индекс диска в драйвере ATA */

/* Буфер на один сектор: на стеке 512 байт держать не хочется,
   стек задачи всего 8 КиБ. */
static u8 secbuf[SECTOR_SIZE];

int kvfs_mounted(void) { return mounted; }

static int read_dir(void) {
    for (int s = 0; s < DIR_SECTORS; s++) {
        if (ata_read(dev, DIR_SECTOR + s, 1, secbuf) < 0) return -1;
        memcpy((u8 *)dir + s * SECTOR_SIZE, secbuf, SECTOR_SIZE);
    }
    return 0;
}

static int write_dir(void) {
    for (int s = 0; s < DIR_SECTORS; s++) {
        memcpy(secbuf, (u8 *)dir + s * SECTOR_SIZE, SECTOR_SIZE);
        if (ata_write(dev, DIR_SECTOR + s, 1, secbuf) < 0) return -1;
    }
    return 0;
}

static int write_super(void) {
    memset(secbuf, 0, SECTOR_SIZE);
    memcpy(secbuf, &super, sizeof(super));
    return ata_write(dev, KVFS_BASE, 1, secbuf);
}

/* Подключение существующей ФС. Возвращает 0, если на диске KvFS. */
int kvfs_mount(void) {
    mounted = 0;
    dev = ata_boot_drive();
    if (dev < 0) return -1;

    if (ata_read(dev, KVFS_BASE, 1, secbuf) < 0) return -2;
    memcpy(&super, secbuf, sizeof(super));
    if (super.magic != KVFS_MAGIC) return -3;      /* чужой или пустой диск */
    if (super.version != KVFS_VERSION) return -4;
    if (read_dir() < 0) return -2;

    mounted = 1;
    return 0;
}

/* Создание новой ФС поверх диска. Стирает каталог, но не данные:
   секторы с телами файлов просто перестают быть кому-то нужны. */
int kvfs_format(void) {
    dev = ata_boot_drive();
    if (dev < 0) return -1;

    u32 total = ata_sectors(dev);
    if (total < DATA_START + 64) return -5;       /* диск неприлично мал */

    memset(&super, 0, sizeof(super));
    super.magic         = KVFS_MAGIC;
    super.version       = KVFS_VERSION;
    super.total_sectors = total;
    super.data_start    = DATA_START;
    super.file_count    = 0;

    memset(dir, 0, sizeof(dir));
    if (write_super() < 0) return -2;
    if (write_dir() < 0)   return -2;

    mounted = 1;
    return 0;
}

static int find_entry(const char *name) {
    for (int i = 0; i < KVFS_MAX_FILES; i++)
        if ((dir[i].flags & 1) && !strcmp(dir[i].name, name)) return i;
    return -1;
}

/* Поиск непрерывного промежутка нужной длины методом «первый подходящий».
   Занятые области отсортированными не хранятся, поэтому идём по
   секторам и для каждого кандидата проверяем пересечения. */
static u32 find_space(u32 need_sectors) {
    u32 candidate = super.data_start;
    u32 limit = super.total_sectors;

    for (int guard = 0; guard < KVFS_MAX_FILES + 1; guard++) {
        u32 next = 0;
        int clash = 0;
        for (int i = 0; i < KVFS_MAX_FILES; i++) {
            if (!(dir[i].flags & 1)) continue;
            u32 a = dir[i].start, b = dir[i].start + dir[i].sectors;
            /* пересекается ли [candidate, candidate+need) с [a, b)? */
            if (candidate < b && a < candidate + need_sectors) {
                clash = 1;
                if (b > next) next = b;
            }
        }
        if (!clash) {
            if (candidate + need_sectors > limit) return 0;   /* места нет */
            return candidate;
        }
        candidate = next;
        if (candidate + need_sectors > limit) return 0;
    }
    return 0;
}

int kvfs_file_count(void) {
    if (!mounted) return 0;
    int n = 0;
    for (int i = 0; i < KVFS_MAX_FILES; i++) if (dir[i].flags & 1) n++;
    return n;
}

/* Перебор файлов: заполняет имя, размер и признак исполняемого.
   Возвращает 0, если запись с таким порядковым номером есть. */
int kvfs_list(int index, char *name, u32 *size, int *is_exec) {
    if (!mounted) return -1;
    int n = 0;
    for (int i = 0; i < KVFS_MAX_FILES; i++) {
        if (!(dir[i].flags & 1)) continue;
        if (n == index) {
            if (name) strncpy(name, dir[i].name, 40);
            if (size) *size = dir[i].size;
            if (is_exec) *is_exec = (dir[i].flags & 2) ? 1 : 0;
            return 0;
        }
        n++;
    }
    return -1;
}

int kvfs_exists(const char *name) {
    if (!mounted) return 0;
    return find_entry(name) >= 0;
}

u32 kvfs_size(const char *name) {
    if (!mounted) return 0;
    int i = find_entry(name);
    return i < 0 ? 0 : dir[i].size;
}

/* Запись файла целиком. Существующий файл с тем же именем заменяется. */
int kvfs_write(const char *name, const void *data, u32 size, int is_exec) {
    if (!mounted) return -1;
    if (!name || !name[0]) return -4;
    if (strlen(name) >= 40) return -5;

    u32 need = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (need == 0) need = 1;

    int slot = find_entry(name);
    if (slot >= 0) {
        /* Перезапись: если новый файл влезает в старое место - используем его */
        if (need <= dir[slot].sectors) {
            dir[slot].size  = size;
            dir[slot].flags = 1 | (is_exec ? 2 : 0);
        } else {
            dir[slot].flags = 0;          /* освобождаем и ищем заново */
            slot = -1;
        }
    }

    if (slot < 0) {
        for (int i = 0; i < KVFS_MAX_FILES; i++)
            if (!(dir[i].flags & 1)) { slot = i; break; }
        if (slot < 0) return -6;          /* каталог переполнен */

        u32 start = find_space(need);
        if (!start) return -7;            /* нет непрерывного места */

        memset(&dir[slot], 0, sizeof(dir[slot]));
        strncpy(dir[slot].name, name, 40);
        dir[slot].name[39] = 0;
        dir[slot].start   = start;
        dir[slot].sectors = need;
        dir[slot].size    = size;
        dir[slot].flags   = 1 | (is_exec ? 2 : 0);
    }

    /* Пишем посекторно: последний сектор дополняем нулями */
    const u8 *p = (const u8 *)data;
    for (u32 s = 0; s < need; s++) {
        u32 chunk = size - s * SECTOR_SIZE;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        memset(secbuf, 0, SECTOR_SIZE);
        if (chunk > 0 && chunk <= SECTOR_SIZE) memcpy(secbuf, p + s * SECTOR_SIZE, chunk);
        if (ata_write(dev, dir[slot].start + s, 1, secbuf) < 0) return -2;
    }

    super.file_count = (u32)kvfs_file_count();
    if (write_dir() < 0) return -2;
    if (write_super() < 0) return -2;
    return 0;
}

/* Чтение файла в буфер. Возвращает число прочитанных байт или < 0. */
int kvfs_read(const char *name, void *buf, u32 max) {
    if (!mounted) return -1;
    int i = find_entry(name);
    if (i < 0) return -3;

    u32 size = dir[i].size;
    if (size > max) size = max;
    u32 need = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    u8 *p = (u8 *)buf;
    for (u32 s = 0; s < need; s++) {
        if (ata_read(dev, dir[i].start + s, 1, secbuf) < 0) return -2;
        u32 chunk = size - s * SECTOR_SIZE;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        memcpy(p + s * SECTOR_SIZE, secbuf, chunk);
    }
    return (int)size;
}

int kvfs_delete(const char *name) {
    if (!mounted) return -1;
    int i = find_entry(name);
    if (i < 0) return -3;
    dir[i].flags = 0;
    super.file_count = (u32)kvfs_file_count();
    if (write_dir() < 0) return -2;
    if (write_super() < 0) return -2;
    return 0;
}

/* Сведения о занятости для команды df и окна установщика */
void kvfs_stats(u32 *total_mb, u32 *used_kb, u32 *files) {
    if (total_mb) *total_mb = mounted ? (super.total_sectors >> 11) : 0;
    if (files)    *files    = mounted ? (u32)kvfs_file_count() : 0;
    if (used_kb) {
        u32 sec = 0;
        if (mounted)
            for (int i = 0; i < KVFS_MAX_FILES; i++)
                if (dir[i].flags & 1) sec += dir[i].sectors;
        *used_kb = sec / 2;              /* 512 байт = полкилобайта */
    }
}

const char *kvfs_error(int code) {
    switch (code) {
        case  0: return "успешно";
        case -1: return "файловая система не подключена";
        case -2: return "ошибка ввода-вывода диска";
        case -3: return "файл не найден";
        case -4: return "пустое имя файла";
        case -5: return "имя длиннее 39 символов";
        case -6: return "каталог переполнен (максимум 64 файла)";
        case -7: return "не хватает непрерывного места на диске";
        default: return "неизвестная ошибка";
    }
}
