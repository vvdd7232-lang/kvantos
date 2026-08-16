/* Простейшая файловая система в оперативной памяти (плоская) */
#include "kernel.h"

static rfile_t files[RAMFS_MAX_FILES];

rfile_t *ramfs_table(void) { return files; }

void ramfs_init(void) {
    memset(files, 0, sizeof(files));

    ramfs_create("readme.txt",
        "KvantOS " KV_VERSION "\n"
        "Учебная 32-битная ОС на собственном ядре, загрузка через GRUB.\n"
        "Наберите help, чтобы увидеть список команд.\n", 0);

    ramfs_create("license.txt",
        "KvantOS распространяется на условиях лицензии MIT.\n"
        "(c) 2026 KvantOS Project. Используйте свободно.\n", 0);

    ramfs_create("poem.txt",
        "  Квант света в регистре живет,\n"
        "  Прерывание тихо зовет,\n"
        "  И стек, как волна, набегает,\n"
        "  А планировщик - все знает.\n", 0);

    ramfs_create("motd.txt",
        "Добро пожаловать в KvantOS!\n"
        "Совет дня: команда 'mem' покажет карту памяти.\n", 0);
}

rfile_t *ramfs_find(const char *name) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0) return &files[i];
    return NULL;
}

int ramfs_create(const char *name, const char *data, u32 size) {
    if (!name || !name[0]) return -4;
    /* Имя длиннее поля молча обрезалось: ramfs_find() потом искал полное
       имя, не находил его и создавал ещё одну запись-дубликат. */
    if (strlen(name) >= sizeof(files[0].name)) return -5;
    if (ramfs_find(name)) return -2;
    if (size == 0 && data) size = (u32)strlen(data);
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) {
            strncpy(files[i].name, name, sizeof(files[i].name));
            files[i].data = (char *)kmalloc(size + 1);
            if (!files[i].data) return -3;
            if (data) memcpy(files[i].data, data, size);
            files[i].data[size] = 0;
            files[i].size = size;
            files[i].used = 1;
            return 0;
        }
    }
    return -1;
}

int ramfs_delete(const char *name) {
    rfile_t *f = ramfs_find(name);
    if (!f) return -1;
    if (f->data) kfree(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}
