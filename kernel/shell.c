/* ============================================================
 *  KvantOS - the command shell (kvsh)
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"

#define CMD_MAX     200
#define ARGS_MAX    16
#define HIST_MAX    16

static char history[HIST_MAX][CMD_MAX];
static int  hist_count = 0;

/* Length of a UTF-8 string in characters (for column alignment) */
static u32 ulen(const char *s) {
    u32 n = 0;
    while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; }
    return n;
}

static void pad_to(const char *s, u32 width) {
    for (u32 i = ulen(s); i < width; i++) kputc(' ');
}

static void prompt(void) {
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs("kvant");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs(":");
    vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    kputs("/");
    vga_set_color(VGA_COLOR(VGA_WHITE, VGA_BLACK));
    kputs("$ ");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

static void hist_add(const char *line) {
    if (!line[0]) return;
    if (hist_count && strcmp(history[(hist_count - 1) % HIST_MAX], line) == 0) return;
    strncpy(history[hist_count % HIST_MAX], line, CMD_MAX);
    hist_count++;
}

/* line input with Backspace and up/down arrow support */
static void readline(char *buf, int max) {
    int len = 0, hpos = hist_count;
    buf[0] = 0;
    for (;;) {
        int ci = kbd_getchar_nb();
        if (ci < 0) { task_yield(); continue; }
        u8 c = (u8)ci;

        if (c == '\n') { kputc('\n'); buf[len] = 0; return; }
        if (c == '\b') {
            if (len) { len--; buf[len] = 0; kputc('\b'); }
            continue;
        }
        if (c == KEY_UP || c == KEY_DOWN) {
            int start = hist_count > HIST_MAX ? hist_count - HIST_MAX : 0;
            if (c == KEY_UP && hpos > start) hpos--;
            else if (c == KEY_DOWN && hpos < hist_count) hpos++;
            else continue;
            while (len) { kputc('\b'); len--; }
            if (hpos < hist_count) {
                strncpy(buf, history[hpos % HIST_MAX], max);
                len = (int)strlen(buf);
                kputs(buf);
            } else { buf[0] = 0; len = 0; }
            continue;
        }
        if (c == 12) { vga_clear(); prompt(); kputs(buf); continue; }   /* Ctrl+L */
        if (c == 3)  { kputs("^C\n"); buf[0] = 0; return; }             /* Ctrl+C */
        if (c < 32 || c > 126) continue;
        if (len < max - 1) { buf[len++] = (char)c; buf[len] = 0; kputc((char)c); }
    }
}

static int split(char *line, char **argv) {
    int argc = 0;
    while (*line && argc < ARGS_MAX) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
    }
    return argc;
}

/* ---------------- commands ---------------- */

/* Length in characters rather than bytes: Cyrillic takes two bytes in
   UTF-8 and strlen would produce twice the indentation. */
static u32 ulen_ascii(const char *s) {
    u32 n = 0;
    while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; }
    return n;
}

/* ============================================================
 *  Disk handling and application installation
 * ============================================================ */
/* Installing the system onto a hard disk. Confirmation is required:
   the operation rewrites the boot sector of the disk. */
/* Switching the interface language on the fly. */
static void cmd_lang(int argc, char **argv)
{
    if (argc < 2) {
        kprintf(T("\n  Interface language: %s\n", "\n  Язык интерфейса: %s\n"), kv_lang_name());
        kputs(T("  Change it with: lang en  |  lang ru\n\n",
                "  Сменить: lang en  |  lang ru\n\n"));
        return;
    }
    if (!strcmp(argv[1], "en") || !strcmp(argv[1], "english")) {
        kv_lang_set(KV_LANG_EN);
        kputs("\n  Language: English\n\n");
    } else if (!strcmp(argv[1], "ru") || !strcmp(argv[1], "russian")) {
        kv_lang_set(KV_LANG_RU);
        kputs("\n  Язык: русский\n\n");
    } else {
        kputs(T("\n  Use: lang en  or  lang ru\n\n",
                "\n  Укажите: lang en  или  lang ru\n\n"));
    }
}

static void cmd_setup(int argc, char **argv) {
    kputs("\n");
    if (!setup_available()) {
        kputs(T("  Installation unavailable: the system was not started from\n", "  Установка недоступна: система запущена не с установочного\n"));
        kputs(T("  install media, no bootloader image found.\n\n", "  носителя, образ загрузчика не найден.\n\n"));
        return;
    }
    if (!ata_present()) {
        kputs(T("  No hard disk found.\n", "  Жёсткий диск не найден.\n"));
        kputs(T("  In VMware add an IDE disk, in QEMU pass -hda.\n\n", "  В VMware добавьте диск типа IDE, в QEMU - параметр -hda.\n\n"));
        return;
    }

    int d = ata_boot_drive();
    int keep = 0;
    int confirmed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--keep"))  keep = 1;
        if (!strcmp(argv[i], "--yes"))   confirmed = 1;
    }

    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  INSTALLING KvantOS ONTO A HARD DISK\n\n", "  УСТАНОВКА KvantOS НА ЖЁСТКИЙ ДИСК\n\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf(T("    disk   : %s\n", "    диск   : %s\n"), ata_model(d));
    kprintf(T("    size   : %u MiB\n", "    объём  : %u МиБ\n"), ata_size_mb(d));
    kprintf(T("    files  : %s\n\n", "    файлы  : %s\n\n"), keep ? T("keep the existing ones", "сохранить существующие") : T("create the filesystem from scratch", "создать файловую систему заново"));

    if (!confirmed) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kputs(T("  WARNING: the boot record of the disk will be overwritten\n", "  ВНИМАНИЕ: загрузочная запись диска будет перезаписана,\n"));
        kputs(T("  and all of its contents will become unreachable.\n\n", "  а всё его содержимое станет недоступно.\n\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputs(T("  If you agree, run:         setup --yes\n", "  Если согласны, выполните:  setup --yes\n"));
        kputs(T("  Keep files on the disk:    setup --yes --keep\n\n", "  Сохранить файлы на диске:  setup --yes --keep\n\n"));
        return;
    }

    kputs(T("  Installation started...\n\n", "  Установка началась...\n\n"));
    int rc = setup_install(keep);
    kprintf("    %s\n", setup_stage_text());

    if (rc == 0) {
        vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        kprintf("\n  %s\n", setup_last_result());
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputs(T("  Remove the media and type reboot.\n\n", "  Извлеките носитель и наберите reboot.\n\n"));
        beep(880, 80); beep(1320, 120);
    } else {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf(T("\n  Failed: %s\n\n", "\n  Не удалось: %s\n\n"), setup_last_result());
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }
}

static void cmd_disk(void) {
    kputs("\n");
    if (!ata_count()) {
        kputs(T("  No ATA disks found.\n", "  Диски ATA не найдены.\n"));
        kputs(T("  In QEMU add: -hda disk.img\n\n", "  В QEMU добавьте: -hda disk.img\n\n"));
        return;
    }
    kprintf(T("  Disks found: %d\n\n", "  Найдено дисков: %d\n\n"), ata_count());
    for (int i = 0; i < ata_count(); i++) {
        kprintf("   [%d] %s\n", i, ata_model(i));
        kprintf(T("       sectors: %u, size: %u MiB%s\n", "       секторов: %u, объём: %u МиБ%s\n"),
                ata_sectors(i), ata_size_mb(i),
                i == ata_boot_drive() ? T("  (system)", "  (системный)") : "");
    }
    kputs("\n");
}

static void cmd_format(void) {
    if (!ata_present()) { kputs(T("\n  No disk found.\n\n", "\n  Диск не найден.\n\n")); return; }
    kputs(T("\n  Formatting the disk for KvFS...\n", "\n  Разметка диска под KvFS...\n"));
    int rc = kvfs_format();
    if (rc == 0) kputs(T("  Done. The filesystem has been created.\n\n", "  Готово. Файловая система создана.\n\n"));
    else kprintf(T("  Failed: %s\n\n", "  Не удалось: %s\n\n"), kvfs_error(rc));
}

static void cmd_df(void) {
    kputs("\n");
    if (!kvfs_mounted()) {
        kputs(T("  The filesystem is not mounted.\n", "  Файловая система не подключена.\n"));
        kputs(T("  Format the disk with the format command.\n\n", "  Разметьте диск командой format.\n\n"));
        return;
    }
    u32 mb, kb, nf;
    kvfs_stats(&mb, &kb, &nf);
    kprintf(T("  KvFS on the disk\n", "  KvFS на диске\n"));
    kprintf(T("    disk size   : %u MiB\n", "    объём диска : %u МиБ\n"), mb);
    kprintf(T("    used        : %u KiB\n", "    занято      : %u КиБ\n"), kb);
    kprintf(T("    files       : %u of 64\n\n", "    файлов      : %u из 64\n\n"), nf);
}

static void cmd_dls(void) {
    kputs("\n");
    if (!kvfs_mounted()) { kputs(T("  The disk is not formatted (format).\n\n", "  Диск не размечен (format).\n\n")); return; }
    char nm[44]; u32 sz; int ex;
    int n = 0;
    kputs(T("     SIZE  TYPE  NAME\n", "   РАЗМЕР  ТИП   ИМЯ\n"));
    for (int i = 0; i < 64; i++) {
        if (kvfs_list(i, nm, &sz, &ex) < 0) break;
        kprintf("  %7u  %s  %s\n", sz, ex ? T("app  ", "прог.") : T("file ", "файл "), nm);
        n++;
    }
    if (!n) kputs(T("  (empty)\n", "  (пусто)\n"));
    kprintf(T("\n  Files: %d\n\n", "\n  Файлов: %d\n\n"), n);
}

static void cmd_dcat(const char *name) {
    if (!kvfs_mounted()) { kputs(T("\n  The disk is not formatted.\n\n", "\n  Диск не размечен.\n\n")); return; }
    u32 sz = kvfs_size(name);
    if (!sz) { kprintf(T("\n  File '%s' not found.\n\n", "\n  Файл '%s' не найден.\n\n"), name); return; }
    if (sz > 8192) sz = 8192;
    char *buf = (char *)kmalloc(sz + 1);
    if (!buf) { kputs(T("\n  Out of memory.\n\n", "\n  Не хватает памяти.\n\n")); return; }
    int got = kvfs_read(name, buf, sz);
    if (got < 0) { kprintf(T("\n  Error: %s\n\n", "\n  Ошибка: %s\n\n"), kvfs_error(got)); kfree(buf); return; }
    buf[got] = 0;
    kputs("\n");
    kputs(buf);
    kputs("\n\n");
    kfree(buf);
}

/* Installation: a file is moved from ramfs onto the disk. The
   "program" flag is set from the .kapp extension - those are exactly
   the files the Programs window shows. */
static void cmd_install(const char *name) {
    if (!kvfs_mounted()) {
        kputs(T("\n  The disk is not formatted. Run format.\n\n", "\n  Диск не размечен. Выполните format.\n\n"));
        return;
    }
    rfile_t *f = ramfs_find(name);
    if (!f) {
        kprintf(T("\n  File '%s' not found in ramfs.\n", "\n  Файл '%s' не найден в ramfs.\n"), name);
        kputs(T("  List them with: ls\n\n", "  Список: ls\n\n"));
        return;
    }
    /* does the name end in .kapp? */
    int is_app = 0;
    u32 l = (u32)strlen(name);
    if (l > 5 && !strcmp(name + l - 5, ".kapp")) is_app = 1;

    int rc = kvfs_write(name, f->data, f->size, is_app);
    if (rc == 0)
        kprintf(T("\n  Installed: %s (%u bytes)%s\n\n", "\n  Установлено: %s (%u байт)%s\n\n"),
                name, f->size, is_app ? T(", application", ", приложение") : "");
    else
        kprintf(T("\n  Failed: %s\n\n", "\n  Не удалось: %s\n\n"), kvfs_error(rc));
}

static void cmd_uninstall(const char *name) {
    if (!kvfs_mounted()) { kputs(T("\n  The disk is not formatted.\n\n", "\n  Диск не размечен.\n\n")); return; }
    int rc = kvfs_delete(name);
    if (rc == 0) kprintf(T("\n  Removed: %s\n\n", "\n  Удалено: %s\n\n"), name);
    else kprintf(T("\n  Failed: %s\n\n", "\n  Не удалось: %s\n\n"), kvfs_error(rc));
}

static void cmd_apps(void) {
    kputs("\n");
    if (!kvfs_mounted()) { kputs(T("  The disk is not formatted (format).\n\n", "  Диск не размечен (format).\n\n")); return; }
    char nm[44]; u32 sz; int ex;
    int n = 0;
    for (int i = 0; i < 64; i++) {
        if (kvfs_list(i, nm, &sz, &ex) < 0) break;
        if (!ex) continue;
        if (!n) kputs(T("  Installed applications:\n\n", "  Установленные приложения:\n\n"));
        /* kprintf cannot left-align (%-24s), so the name is padded
           with spaces by hand. */
        kprintf("   %s", nm);
        for (int pad = (int)ulen_ascii(nm); pad < 24; pad++) kputc(' ');
        kprintf(T(" %u KiB\n", " %u КиБ\n"), (sz + 1023) / 1024);
        n++;
    }
    if (!n) {
        kputs(T("  No applications.\n", "  Приложений нет.\n"));
        kputs(T("  Build a sample: make in sdk/, then install name.kapp\n", "  Соберите пример: sdk/make, затем install имя.kapp\n"));
    }
    kputs(T("\n  To run them: graphics mode (guimenu), the Programs icon.\n\n", "\n  Запуск: графический режим (guimenu), значок «Программы».\n\n"));
}

static void cmd_help(void) {
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("\n  KvantOS commands\n", "\n  Команды KvantOS\n"));
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  ──────────────────────────────────────────────────────────\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    struct { const char *c, *d; } t[] = {
        {"help",      T("show this help", "показать эту справку")},
        {"about",     T("system information and logo", "сведения о системе и логотип")},
        {"clear",     T("clear the screen (Ctrl+L)", "очистить экран (Ctrl+L)")},
        {"echo TEXT", T("print text", "вывести текст")},
        {"mem",       T("memory and heap statistics", "статистика памяти и кучи")},
        {"cpu",       T("CPU information", "информация о процессоре")},
        {"uptime",    T("uptime and timer ticks", "время работы и тики таймера")},
        {"date",      T("date and time from CMOS/RTC", "дата и время из CMOS/RTC")},
        {"ps",        T("list scheduler tasks", "список задач планировщика")},
        {"spawn N",   T("spawn N background counter tasks", "создать N фоновых задач-счётчиков")},
        {"ls",        T("list files in ramfs", "список файлов в ramfs")},
        {"cat FILE",  T("show the contents of a file", "показать содержимое файла")},
        {"write F T", T("create file F containing text T", "создать файл F с текстом T")},
        {"rm FILE",   T("delete a file", "удалить файл")},
        {"guimenu",   T("start graphics mode (mouse + windows)", "запустить графический режим (мышь + окна)")},
        {"setup",     T("INSTALL the system onto a hard disk", "УСТАНОВИТЬ систему на жёсткий диск")},
        {"disk",      T("information about ATA disks", "сведения о дисках ATA")},
        {"format",    T("format the disk for KvFS (erases data!)", "разметить диск под KvFS (стирает данные!)")},
        {"df",        T("disk usage", "занятость диска")},
        {"dls",       T("files on the disk", "файлы на диске")},
        {"dcat F",    T("show a file from the disk", "показать файл с диска")},
        {"mount",     T("mounted volumes: FAT32, NTFS, KvFS", "подключённые тома: FAT32, NTFS, KvFS")},
        {"rescan",    T("scan the disks for volumes again", "заново просканировать диски")},
        {"vls PATH",  T("list a directory on any volume", "список каталога на любом томе")},
        {"vcd PATH",  T("change the current volume directory", "сменить текущий каталог тома")},
        {"vcat PATH", T("show a file from any volume", "показать файл с любого тома")},
        {"vcp A B",   T("copy a file between volumes", "скопировать файл между томами")},
        {"vrm PATH",  T("delete a file or empty directory", "удалить файл или пустой каталог")},
        {"vmkdir P",  T("create a directory", "создать каталог")},
        {"install F", T("install an application from ramfs to disk", "поставить приложение из ramfs на диск")},
        {"uninstall F",T("remove an application from the disk", "удалить приложение с диска")},
        {"apps",      T("list installed applications", "список установленных приложений")},
        {"lspci [-v]",T("devices on the PCI bus", "устройства на шине PCI")},
        {"gpu",       T("graphics adapter and driver information", "сведения о видеокарте и драйвере")},
        {"vidmode",   T("list and change the screen resolution", "список и смена разрешения экрана")},
        {"refresh N", T("screen refresh rate in Hz", "частота обновления экрана в Гц")},
        {"gfx",       T("video mode and framebuffer information", "сведения о видеорежиме и фреймбуфере")},
        {"hwreport",  T("full hardware report (copied to COM1)", "полный отчёт о железе (дубль в COM1)")},
        {"colors",    T("VGA palette (16 colours)", "палитра VGA (16 цветов)")},
        {"lang [en|ru]", T("interface language", "язык интерфейса")},
        {T("beep [Hz]", "beep [Гц]"), T("sound through the PC speaker", "звук через PC-спикер")},
        {"alloc N",   T("allocate N bytes on the heap (test)", "выделить N байт в куче (тест)")},
        {"crash",     T("raise an exception (panic test)", "вызвать исключение (тест паники)")},
        {"reboot",    T("reboot the machine", "перезагрузить машину")},
        {"poweroff",  T("power off (ACPI)", "выключить питание (ACPI)")},
    };
    for (u32 i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
        kprintf("  %s", t[i].c);
        pad_to(t[i].c, 12);
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kprintf("%s\n", t[i].d);
    }
    kputc('\n');
}

void logo_print(void) {
    vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    kputs("\n");
    kputs("   ██╗  ██╗██╗   ██╗ █████╗ ███╗   ██╗████████╗\n");
    kputs("   ██║ ██╔╝██║   ██║██╔══██╗████╗  ██║╚══██╔══╝\n");
    kputs("   █████╔╝ ██║   ██║███████║██╔██╗ ██║   ██║   \n");
    kputs("   ██╔═██╗ ╚██╗ ██╔╝██╔══██║██║╚██╗██║   ██║   \n");
    kputs("   ██║  ██╗ ╚████╔╝ ██║  ██║██║ ╚████║   ██║   \n");
    kputs("   ╚═╝  ╚═╝  ╚═══╝  ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝   \n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

static void cmd_about(void) {
    logo_print();
    vga_set_color(VGA_COLOR(VGA_WHITE, VGA_BLACK));
    kprintf("\n   %s %s\n", KV_NAME, KV_VERSION);
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf(T("   Architecture: %s\n", "   Архитектура : %s\n"), KV_ARCH);
    kprintf(T("   Bootloader  : GRUB 2 (Multiboot 1)\n", "   Загрузчик   : GRUB 2 (Multiboot 1)\n"));
    kprintf(T("   Kernel      : monolithic, written from scratch\n", "   Ядро        : монолитное, собственной разработки\n"));
    kprintf(T("   Build       : %s\n", "   Сборка      : %s\n"), KV_BUILD);
    kprintf(T("   Subsystems  : GDT/IDT, PIC, PIT, PS/2, RTC, VGA, COM1,\n", "   Подсистемы  : GDT/IDT, PIC, PIT, PS/2, RTC, VGA, COM1,\n"));
    kprintf(T("                 PMM, paging, heap, scheduler, ramfs\n\n", "                 PMM, paging, куча, планировщик, ramfs\n\n"));
}

static void cmd_mem(void) {
    u32 total = pmm_total_bytes();
    u32 used  = pmm_used_frames() * 4096;
    u32 ht, hu, hb;
    heap_stats(&ht, &hu, &hb);

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  Physical memory\n", "  Физическая память\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf(T("    total   : %u KiB (%u MiB), %u pages\n", "    всего   : %u КиБ (%u МиБ), страниц %u\n"), total / 1024, total / 1048576, pmm_total_frames());
    kprintf(T("    used    : %u KiB (%u pages)\n", "    занято  : %u КиБ (%u страниц)\n"), used / 1024, pmm_used_frames());
    kprintf(T("    free    : %u KiB\n", "    свободно: %u КиБ\n"), (total - used) / 1024);

    /* the fill bar */
    u32 pct = total ? (used * 100u) / total : 0;
    kputs("    [");
    for (u32 i = 0; i < 40; i++) {
        if (i * 100u / 40u < pct) { vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK)); kputs("█"); }
        else { vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK)); kputs("░"); }
    }
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("] %u%%\n", pct);

    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("\n  Kernel heap\n", "\n  Куча ядра\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf(T("    size    : %u KiB\n", "    размер  : %u КиБ\n"), ht / 1024);
    kprintf(T("    used    : %u bytes\n", "    занято  : %u байт\n"), hu);
    kprintf(T("    blocks  : %u\n", "    блоков  : %u\n"), hb);
    kprintf(T("    kernel  : %p .. %p\n\n", "    ядро    : %p .. %p\n\n"), (void *)&kernel_start, (void *)&kernel_end);
}

static void cmd_cpu(void) {
    char vendor[16], brand[52];
    cpu_vendor(vendor);
    cpu_brand(brand);
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  Processor\n", "  Процессор\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf(T("    vendor : %s\n", "    вендор : %s\n"), vendor);
    kprintf(T("    model  : %s\n", "    модель : %s\n"), brand);
    kprintf(T("    mode   : protected, 32-bit, paging enabled\n\n", "    режим  : защищённый, 32 бита, страничная адресация вкл.\n\n"));
}

static void cmd_uptime(void) {
    u32 s = timer_seconds();
    kprintf(T("\n  Uptime: %uh %um %us\n", "\n  Время работы: %u ч %u мин %u с\n"), s / 3600, (s / 60) % 60, s % 60);
    kprintf(T("  Timer ticks: %u (at %u Hz)\n\n", "  Тиков таймера: %u (частота %u Гц)\n\n"), (u32)timer_ticks(), timer_hz());
}

static void cmd_date(void) {
    rtc_time_t t;
    rtc_read(&t);
    const char *mn[] = {"", T("January", "января"), T("February", "февраля"), T("March", "марта"), T("April", "апреля"), T("May", "мая"), T("June", "июня"),
                        T("July", "июля"), T("August", "августа"), T("September", "сентября"), T("October", "октября"), T("November", "ноября"), T("December", "декабря")};
    kprintf(T("\n  %02u:%02u:%02u, %u %s %u (UTC)\n\n", "\n  %02u:%02u:%02u, %u %s %u года (UTC)\n\n"),
            t.hour, t.min, t.sec, t.day,
            (t.month >= 1 && t.month <= 12) ? mn[t.month] : "?", t.year);
}

static void cmd_ps(void) {
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("   ID  NAME             STATE        SWITCHES\n", "   ID  ИМЯ              СОСТОЯНИЕ    ПЕРЕКЛЮЧЕНИЙ\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    task_t *cur = task_current();
    task_t *t = cur;
    int guard = 64;
    do {
        const char *st = t->state == TASK_READY ? T("ready", "готова") :
                         t->state == TASK_SLEEPING ? T("sleeping", "спит") : T("finished", "завершена");
        if (t == cur) vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        kprintf("   %u", t->id);
        for (u32 s = t->id > 99 ? 3 : (t->id > 9 ? 2 : 1); s < 4; s++) kputc(' ');
        kprintf("%s", t->name);
        pad_to(t->name, 17);
        kprintf("%s", st);
        pad_to(st, 13);
        kprintf("%u%s\n", t->switches, t == cur ? T("  <- current", "  <- текущая") : "");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        t = t->next;
    } while (t != cur && guard--);
    kprintf(T("\n  Active tasks in total: %u\n\n", "\n  Всего активных задач: %u\n\n"), task_count());
}

/* a background counter task: writes a dot into the status line through a global counter */
static volatile u32 worker_ticks = 0;
static void worker_task(void) {
    for (int i = 0; i < 1000; i++) {
        worker_ticks++;
        task_sleep(250);
    }
}

static void cmd_spawn(int n) {
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        char name[16];
        ksnprintf(name, sizeof(name), "worker%d", i + 1);
        task_t *t = task_create(name, worker_task);
        if (t) kprintf(T("  task #%u created (%s)\n", "  создана задача #%u (%s)\n"), t->id, t->name);
        else   kputs(T("  could not create the task\n", "  не удалось создать задачу\n"));
    }
    kputc('\n');
}

static void cmd_ls(void) {
    rfile_t *tbl = ramfs_table();
    u32 count = 0, bytes = 0;
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("     SIZE  NAME\n", "   РАЗМЕР  ИМЯ\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!tbl[i].used) continue;
        kprintf("   %6u  ", tbl[i].size);
        vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
        kprintf("%s\n", tbl[i].name);
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        count++; bytes += tbl[i].size;
    }
    kprintf(T("\n  Files: %u, %u bytes in total (ramfs)\n\n", "\n  Файлов: %u, суммарно %u байт (ramfs)\n\n"), count, bytes);
}

static void cmd_cat(const char *name) {
    rfile_t *f = ramfs_find(name);
    if (!f) { kprintf(T("  cat: file '%s' not found\n\n", "  cat: файл '%s' не найден\n\n"), name); return; }
    kputc('\n');
    kputs(f->data);
    if (f->size && f->data[f->size - 1] != '\n') kputc('\n');
    kputc('\n');
}

static void cmd_colors(void) {
    const char *names[] = {T("black", "чёрный"),T("blue", "синий"),T("green", "зелёный"),T("cyan", "бирюзовый"),T("red", "красный"),T("magenta", "пурпурный"),
                           T("brown", "коричневый"),T("lt.gray", "св.серый"),T("dk.gray", "т.серый"),T("lt.blue", "св.синий"),T("lt.green", "св.зелёный"),
                           T("lt.cyan", "св.бирюз."),T("lt.red", "св.красный"),T("pink", "розовый"),T("yellow", "жёлтый"),T("white", "белый")};
    kputc('\n');
    for (int i = 0; i < 16; i++) {
        vga_set_color(VGA_COLOR(i == 0 ? VGA_WHITE : (u8)i, i == 0 ? VGA_BLUE : VGA_BLACK));
        kprintf("  %2d ██ %s", i, names[i]);
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        if (i % 2) kputc('\n');
        else pad_to(names[i], 14);
    }
    kputc('\n');
}

/* A summary hardware report: duplicated on COM1 so that it can be
   collected from a machine whose screen is not visible. */
static void cmd_hwreport(void) {
    char vend[16], brand[52];
    cpu_vendor(vend);
    cpu_brand(brand);

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  Configuration report\n", "  Отчёт о конфигурации\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    kprintf(T("    OS         : %s %s\n", "    ОС         : %s %s\n"), KV_NAME, KV_VERSION);
    kprintf(T("    processor  : %s\n", "    процессор  : %s\n"), brand);
    kprintf(T("    CPU vendor : %s\n", "    вендор CPU : %s\n"), vend);
    kprintf(T("    RAM        : %u MiB, %u pages\n", "    ОЗУ        : %u МиБ, страниц %u\n"),
            pmm_total_bytes() / 1048576, pmm_total_frames());
    kprintf(T("    PCI devices   : %u\n", "    устройств PCI : %u\n"), pci_count());

    pci_dev_t *g = pci_gpu();
    if (g)
        kprintf(T("    adapter    : %s (%04x:%04x)\n", "    видеокарта : %s (%04x:%04x)\n"),
                pci_gpu_model(g->vendor, g->device), g->vendor, g->device);
    else
        kputs(T("    adapter    : not found on the PCI bus\n", "    видеокарта : не найдена на шине PCI\n"));

    kprintf(T("    video mode : ", "    видеорежим : "));
    if (fb_active())
        kprintf(T("%u x %u, %u bpp, pitch %u\n", "%u x %u, %u бит, шаг %u\n"),
                fb_width(), fb_height(), fb_bpp_get(), fb_pitch_get());
    else
        kputs(T("VGA text 80x25\n", "текстовый VGA 80x25\n"));

    kprintf(T("    VBE interface : %s\n", "    интерфейс VBE : %s\n"), vbe_backend_name());
    kprintf(T("    PS/2 mouse : %s\n", "    мышь PS/2  : %s\n"), mouse_present() ? T("present", "есть") : T("no", "нет"));

    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs(T("    the report is duplicated on COM1 (38400 8N1)\n\n", "    отчёт продублирован в COM1 (38400 8N1)\n\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

static void cmd_gfx(void) {
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  Video subsystem\n", "  Видеоподсистема\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    if (!fb_active()) {
        kputs(T("    linear framebuffer: unavailable\n", "    линейный фреймбуфер: недоступен\n"));
        kputs(T("    VGA text mode 80x25 is active\n\n", "    активен текстовый режим VGA 80x25\n\n"));
        return;
    }
    kprintf(T("    resolution : %u x %u\n", "    разрешение : %u x %u\n"), fb_width(), fb_height());
    kprintf(T("    depth      : %u bits per pixel\n", "    глубина    : %u бит на пиксель\n"), fb_bpp_get());
    kprintf(T("    pitch      : %u bytes\n", "    шаг строки : %u байт\n"), fb_pitch_get());
    kprintf(T("    address    : %p (%u KiB)\n", "    адрес      : %p (%u КиБ)\n"), (void *)fb_base(), fb_bytes() / 1024);
    kprintf(T("    PS/2 mouse : %s\n", "    мышь PS/2  : %s\n"), mouse_present() ? T("detected", "обнаружена") : T("no", "нет"));
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs(T("    type guimenu to start the desktop environment\n\n", "    наберите guimenu для запуска графической среды\n\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

static void cmd_guimenu(void) {
    if (!fb_active()) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kputs(T("\n  Graphics mode unavailable: the bootloader provided no\n", "\n  Графический режим недоступен: загрузчик не предоставил\n"));
        kputs(T("  linear framebuffer. Pick the normal GRUB menu entry.\n\n", "  линейный фреймбуфер. Выберите обычный пункт меню GRUB.\n\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }
    kputs(T("\n  Switching to graphics mode...\n", "\n  Переключение в графический режим...\n"));
    kputs(T("  Back to the console: Esc or Q\n", "  Выход обратно в консоль: Esc или Q\n"));
    sleep_ms(400);

    int grc = gui_run();

    /* back to text mode */
    vga_text_mode_restore();
    vga_clear();
    logo_print();
    if (grc == -2) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kputs(T("\n  Not enough kernel memory for a screen buffer at this\n", "\n  Не хватает памяти ядра под буфер экрана при текущем\n"));
        kputs(T("  resolution. Lower it, e.g.: vidmode 1024 768\n\n", "  разрешении. Уменьшите его, например: vidmode 1024 768\n\n"));
    }
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs(T("\n  Back in the kvsh text console.\n\n", "\n  Возврат в текстовую консоль kvsh.\n\n"));
}

static void cmd_alloc(int n) {
    if (n <= 0) { kputs(T("  usage: alloc <bytes>\n\n", "  использование: alloc <байт>\n\n")); return; }
    u32 ht, hu, hb;
    heap_stats(&ht, &hu, &hb);
    if ((u32)n > ht) {
        kprintf(T("  more than the heap size was requested (%u KiB)\n\n", "  запрошено больше размера кучи (%u КиБ)\n\n"), ht / 1024);
        return;
    }
    void *p = kmalloc((size_t)n);
    if (!p) { kputs(T("  kmalloc returned NULL (out of memory)\n\n", "  kmalloc вернул NULL (не хватило памяти)\n\n")); return; }
    memset(p, 0xAA, (size_t)n);
    kprintf(T("  allocated %d bytes at %p (phys. %p)\n", "  выделено %d байт по адресу %p (физ. %p)\n"), n, p, (void *)paging_phys((u32)p));
    kfree(p);
    kputs(T("  block released\n\n", "  блок освобождён\n\n"));
}

/* ---------- volumes: FAT32, NTFS and everything else the VFS knows ---------- */

/* Turn a user-typed path into an absolute one. A bare name is taken
   relative to the current volume path kept below. */
static char sh_cwd[VFS_MAX_PATH] = "";

static void sh_abs(char *dst, u32 dstsz, const char *arg) {
    if (arg[0] == '/') { strncpy(dst, arg, dstsz); return; }
    if (!sh_cwd[0]) { ksnprintf(dst, dstsz, "/mnt/%s", arg); return; }
    vfs_join(dst, dstsz, sh_cwd, arg);
}

static void cmd_mount(void) {
    int n = vfs_volume_count();
    if (!n) {
        kputs(T("  No volumes mounted.\n\n", "  Нет подключённых томов.\n\n"));
        return;
    }
    kputs(T("\n  Volume      Type    Label             Size      Free   Access\n",
            "\n  Том         Тип     Метка             Размер    Своб.  Доступ\n"));
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  ──────────────────────────────────────────────────────────────\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    for (int i = 0; i < n; i++) {
        vfs_volume_t *v = vfs_volume(i);
        char path[VFS_MAX_PATH];
        ksnprintf(path, sizeof(path), "/mnt/%s", v->name);
        u32 tk = 0, fk = 0;
        vfs_space(path, &tk, &fk);

        kprintf("  /mnt/%s", v->name);
        for (u32 k = strlen(v->name); k < 7; k++) kputs(" ");
        kprintf(" %s", vfs_kind_name(v->kind));
        for (u32 k = strlen(vfs_kind_name(v->kind)); k < 7; k++) kputs(" ");
        kprintf(" %s", v->label);
        for (u32 k = utf8_len(v->label); k < 18; k++) kputs(" ");
        if (tk >= 1024) kprintf("%u MiB", tk / 1024); else kprintf("%u KiB", tk);
        kputs("  ");
        if (fk >= 1024) kprintf("%u MiB", fk / 1024); else kprintf("%u KiB", fk);
        kputs(v->writable ? T("  rw\n", "  чт+зп\n") : T("  ro\n", "  только чт\n"));
    }
    kputs("\n");
}

static void cmd_vls(int argc, char **argv) {
    char path[VFS_MAX_PATH];
    if (argc > 1) sh_abs(path, sizeof(path), argv[1]);
    else if (sh_cwd[0]) strncpy(path, sh_cwd, sizeof(path));
    else { kputs(T("  usage: vls /mnt/hda1[/dir]\n\n", "  использование: vls /mnt/hda1[/каталог]\n\n")); return; }

    if (vfs_stat(path) != 2) {
        kprintf(T("  Not a directory: %s\n\n", "  Не каталог: %s\n\n"), path);
        return;
    }

    kprintf("\n  %s\n", path);
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  ──────────────────────────────────────────────────────────\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    vfs_dirent_t e;
    int n = 0, dirs = 0;
    u32 bytes = 0;
    for (int i = 0; vfs_readdir(path, i, &e); i++) {
        if (e.is_dir) {
            vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
            kprintf("  %s/", e.name);
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
            kputs("\n");
            dirs++;
        } else {
            kprintf("  %s", e.name);
            for (u32 k = utf8_len(e.name); k < 40; k++) kputs(" ");
            kprintf("%u\n", e.size);
            bytes += e.size;
        }
        n++;
        if (n > 400) { kputs(T("  ... (truncated)\n", "  ... (список обрезан)\n")); break; }
    }
    kprintf(T("  %d entries, %d directories, %u bytes\n\n",
              "  объектов: %d, каталогов: %d, байт: %u\n\n"), n, dirs, bytes);
}

static void cmd_vcat(int argc, char **argv) {
    if (argc < 2) { kputs(T("  usage: vcat <path>\n\n", "  использование: vcat <путь>\n\n")); return; }
    char path[VFS_MAX_PATH];
    sh_abs(path, sizeof(path), argv[1]);

    if (vfs_stat(path) != 1) {
        kprintf(T("  File not found: %s\n\n", "  Файл не найден: %s\n\n"), path);
        return;
    }

    u32 size = vfs_size(path);
    u32 show = size > 4096 ? 4096 : size;
    char *buf = (char *)kmalloc(show + 1);
    if (!buf) { kputs(T("  Not enough memory\n\n", "  Недостаточно памяти\n\n")); return; }

    int got = vfs_read(path, 0, buf, show);
    if (got < 0) { kfree(buf); kputs(T("  Read error\n\n", "  Ошибка чтения\n\n")); return; }
    buf[got] = 0;

    kputs("\n");
    for (int i = 0; i < got; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\t' || (u8)c >= 32) kputc(c);
        else kputc('.');
    }
    kputs("\n");
    if (size > show)
        kprintf(T("  ... shown %u of %u bytes\n", "  ... показано %u из %u байт\n"), show, size);
    kputs("\n");
    kfree(buf);
}

static void cmd_vcp(int argc, char **argv) {
    if (argc < 3) {
        kputs(T("  usage: vcp <source> <destination>\n\n",
                "  использование: vcp <источник> <приёмник>\n\n"));
        return;
    }
    char from[VFS_MAX_PATH], to[VFS_MAX_PATH];
    sh_abs(from, sizeof(from), argv[1]);
    sh_abs(to,   sizeof(to),   argv[2]);

    /* copying onto a directory keeps the original name */
    if (vfs_stat(to) == 2) {
        const char *base = argv[1];
        for (const char *p = argv[1]; *p; p++) if (*p == '/') base = p + 1;
        char joined[VFS_MAX_PATH];
        vfs_join(joined, sizeof(joined), to, base);
        strncpy(to, joined, sizeof(to));
    }

    if (vfs_stat(from) != 1) { kprintf(T("  Source not found: %s\n\n", "  Источник не найден: %s\n\n"), from); return; }

    u32 size = vfs_size(from);
    if (size > 4u * 1024 * 1024) {
        kputs(T("  The file is larger than 4 MiB - not supported yet\n\n",
                "  Файл больше 4 МиБ — пока не поддерживается\n\n"));
        return;
    }
    u8 *buf = (u8 *)kmalloc(size ? size : 1);
    if (!buf) { kputs(T("  Not enough memory\n\n", "  Недостаточно памяти\n\n")); return; }

    int got = vfs_read(from, 0, buf, size);
    if (got < 0) { kfree(buf); kputs(T("  Read error\n\n", "  Ошибка чтения\n\n")); return; }

    int wrote = vfs_write(to, buf, (u32)got);
    kfree(buf);

    if (wrote == (int)got) kprintf(T("  Copied %u bytes -> %s\n\n", "  Скопировано %u байт -> %s\n\n"), (u32)got, to);
    else if (wrote == -2)  kputs(T("  The target is read-only\n\n", "  Приёмник только для чтения\n\n"));
    else if (wrote == -3)  kputs(T("  Not enough space\n\n", "  Недостаточно места\n\n"));
    else                   kputs(T("  Write error\n\n", "  Ошибка записи\n\n"));
}

static void cmd_vrm(int argc, char **argv) {
    if (argc < 2) { kputs(T("  usage: vrm <path>\n\n", "  использование: vrm <путь>\n\n")); return; }
    char path[VFS_MAX_PATH];
    sh_abs(path, sizeof(path), argv[1]);

    int r = vfs_remove(path);
    if (r == 0)       kprintf(T("  Deleted: %s\n\n", "  Удалено: %s\n\n"), path);
    else if (r == -2) kputs(T("  This volume is read-only\n\n", "  Этот том только для чтения\n\n"));
    else if (r == -4) kputs(T("  The directory is not empty\n\n", "  Каталог не пуст\n\n"));
    else              kputs(T("  Could not delete\n\n", "  Не удалось удалить\n\n"));
}

static void cmd_vmkdir(int argc, char **argv) {
    if (argc < 2) { kputs(T("  usage: vmkdir <path>\n\n", "  использование: vmkdir <путь>\n\n")); return; }
    char path[VFS_MAX_PATH];
    sh_abs(path, sizeof(path), argv[1]);

    int r = vfs_mkdir(path);
    if (r == 0)       kprintf(T("  Created: %s\n\n", "  Создан: %s\n\n"), path);
    else if (r == -2) kputs(T("  This volume is read-only\n\n", "  Этот том только для чтения\n\n"));
    else if (r == -5) kputs(T("  Such a name already exists\n\n", "  Такое имя уже существует\n\n"));
    else              kputs(T("  Could not create the directory\n\n", "  Не удалось создать каталог\n\n"));
}

static void cmd_vcd(int argc, char **argv) {
    if (argc < 2) {
        kprintf(T("  Current: %s\n\n", "  Текущий: %s\n\n"), sh_cwd[0] ? sh_cwd : "/mnt");
        return;
    }
    char path[VFS_MAX_PATH];
    if (!strcmp(argv[1], "..")) {
        vfs_parent(path, sizeof(path), sh_cwd[0] ? sh_cwd : "/mnt");
    } else {
        sh_abs(path, sizeof(path), argv[1]);
    }
    if (vfs_stat(path) != 2) {
        kprintf(T("  Not a directory: %s\n\n", "  Не каталог: %s\n\n"), path);
        return;
    }
    strncpy(sh_cwd, path, sizeof(sh_cwd));
    kprintf("  %s\n\n", sh_cwd);
}

static void cmd_rescan(void) {
    vfs_init();
    int n = vfs_autoscan();
    kprintf(T("  Volumes found: %d\n\n", "  Найдено томов: %d\n\n"), n);
    cmd_mount();
}


/* ---------------- main loop ---------------- */

static void status_task(void) {
    for (;;) {
        char right[64];
        u32 s = timer_seconds();
        u32 total = pmm_total_bytes() / 1048576;
        u32 used  = pmm_used_frames() * 4096 / 1048576;
        ksnprintf(right, sizeof(right), T("RAM %u/%u MiB | tasks %u | %02u:%02u:%02u", "ОЗУ %u/%u МиБ | задач %u | %02u:%02u:%02u"),
                  used, total, task_count(), s / 3600, (s / 60) % 60, s % 60);
        vga_status(" KvantOS " KV_VERSION "  —  kvsh", right,
                   VGA_COLOR(VGA_WHITE, VGA_BLUE));
        task_sleep(500);
    }
}

void shell_run(void) {
    char line[CMD_MAX];
    char *argv[ARGS_MAX];

    task_create("status", status_task);

    logo_print();
    vga_set_color(VGA_COLOR(VGA_WHITE, VGA_BLACK));
    kprintf(T("\n   Welcome to %s %s!\n", "\n   Добро пожаловать в %s %s!\n"), KV_NAME, KV_VERSION);
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs(T("   Type ", "   Наберите "));
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs("help");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs(T(" for the list of commands, ", " для списка команд, "));
    vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    kputs("guimenu");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs(T(" — graphics mode.\n\n", " — графический режим.\n\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    for (;;) {
        prompt();
        readline(line, CMD_MAX);
        if (!line[0]) continue;
        hist_add(line);

        char work[CMD_MAX];
        strncpy(work, line, CMD_MAX);
        int argc = split(work, argv);
        if (!argc) continue;

        /* The command name is lowercased: with CapsLock on it would
           otherwise arrive as "MEM" and no command would be found.
           Arguments (file names, text) are left untouched. */
        for (char *q = argv[0]; *q; q++)
            if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');

        const char *cmd = argv[0];

        if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmd_help();
        else if (!strcmp(cmd, "about") || !strcmp(cmd, "ver")) cmd_about();
        else if (!strcmp(cmd, "clear") || !strcmp(cmd, "cls")) vga_clear();
        else if (!strcmp(cmd, "echo")) {
            for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
            kputs("\n");
        }
        else if (!strcmp(cmd, "setup") || !strcmp(cmd, "install-system")) cmd_setup(argc, argv);
        else if (!strcmp(cmd, "disk")) cmd_disk();
        else if (!strcmp(cmd, "format")) cmd_format();
        else if (!strcmp(cmd, "df")) cmd_df();
        else if (!strcmp(cmd, "dls")) cmd_dls();
        else if (!strcmp(cmd, "dcat")) {
            if (argc < 2) kputs(T("\n  Usage: dcat NAME\n\n", "\n  Использование: dcat ИМЯ\n\n"));
            else cmd_dcat(argv[1]);
        }
        else if (!strcmp(cmd, "install")) {
            if (argc < 2) kputs(T("\n  Usage: install NAME\n\n", "\n  Использование: install ИМЯ\n\n"));
            else cmd_install(argv[1]);
        }
        else if (!strcmp(cmd, "uninstall")) {
            if (argc < 2) kputs(T("\n  Usage: uninstall NAME\n\n", "\n  Использование: uninstall ИМЯ\n\n"));
            else cmd_uninstall(argv[1]);
        }
        else if (!strcmp(cmd, "apps")) cmd_apps();
        else if (!strcmp(cmd, "mount") || !strcmp(cmd, "volumes")) cmd_mount();
        else if (!strcmp(cmd, "rescan")) cmd_rescan();
        else if (!strcmp(cmd, "vls")) cmd_vls(argc, argv);
        else if (!strcmp(cmd, "vcd")) cmd_vcd(argc, argv);
        else if (!strcmp(cmd, "vcat")) cmd_vcat(argc, argv);
        else if (!strcmp(cmd, "vcp")) cmd_vcp(argc, argv);
        else if (!strcmp(cmd, "vrm")) cmd_vrm(argc, argv);
        else if (!strcmp(cmd, "vmkdir")) cmd_vmkdir(argc, argv);
        else if (!strcmp(cmd, "mem") || !strcmp(cmd, "free")) cmd_mem();
        else if (!strcmp(cmd, "cpu")) cmd_cpu();
        else if (!strcmp(cmd, "uptime")) cmd_uptime();
        else if (!strcmp(cmd, "date") || !strcmp(cmd, "time")) cmd_date();
        else if (!strcmp(cmd, "ps") || !strcmp(cmd, "tasks")) cmd_ps();
        else if (!strcmp(cmd, "spawn")) cmd_spawn(argc > 1 ? atoi(argv[1]) : 1);
        else if (!strcmp(cmd, "ls") || !strcmp(cmd, "dir")) cmd_ls();
        else if (!strcmp(cmd, "cat")) {
            if (argc < 2) kputs(T("  usage: cat <file>\n\n", "  использование: cat <файл>\n\n"));
            else cmd_cat(argv[1]);
        }
        else if (!strcmp(cmd, "write")) {
            if (argc < 3) kputs(T("  usage: write <file> <text...>\n\n", "  использование: write <файл> <текст...>\n\n"));
            else {
                char body[CMD_MAX];
                body[0] = 0;
                u32 pos = 0;
                for (int i = 2; i < argc; i++) {
                    u32 l = (u32)strlen(argv[i]);
                    if (pos + l + 2 >= CMD_MAX) break;
                    memcpy(body + pos, argv[i], l); pos += l;
                    body[pos++] = (i + 1 < argc) ? ' ' : '\n';
                }
                body[pos] = 0;
                int r = ramfs_create(argv[1], body, pos);
                if (r == 0) kprintf(T("  file '%s' created (%u bytes)\n\n", "  создан файл '%s' (%u байт)\n\n"), argv[1], pos);
                else if (r == -2) kprintf(T("  file '%s' already exists\n\n", "  файл '%s' уже существует\n\n"), argv[1]);
                else if (r == -5) kputs(T("  file name too long (23 characters maximum)\n\n", "  имя файла слишком длинное (максимум 23 символа)\n\n"));
                else if (r == -4) kputs(T("  empty file name\n\n", "  пустое имя файла\n\n"));
                else if (r == -3) kputs(T("  not enough memory on the heap\n\n", "  не хватает памяти в куче\n\n"));
                else kputs(T("  ramfs is full\n\n", "  ramfs переполнена\n\n"));
            }
        }
        else if (!strcmp(cmd, "rm") || !strcmp(cmd, "del")) {
            if (argc < 2) kputs(T("  usage: rm <file>\n\n", "  использование: rm <файл>\n\n"));
            else if (ramfs_delete(argv[1]) == 0) kprintf(T("  file '%s' deleted\n\n", "  файл '%s' удалён\n\n"), argv[1]);
            else kprintf(T("  rm: '%s' not found\n\n", "  rm: '%s' не найден\n\n"), argv[1]);
        }
        else if (!strcmp(cmd, "guimenu") || !strcmp(cmd, "gui") || !strcmp(cmd, "startx")) cmd_guimenu();
        else if (!strcmp(cmd, "gfx") || !strcmp(cmd, "video")) cmd_gfx();
        else if (!strcmp(cmd, "hwreport") || !strcmp(cmd, "hw")) cmd_hwreport();
        else if (!strcmp(cmd, "lspci")) cmd_lspci(argc > 1 && !strcmp(argv[1], "-v"));
        else if (!strcmp(cmd, "gpu") || !strcmp(cmd, "gpuinfo")) cmd_gpuinfo();
        else if (!strcmp(cmd, "vidmode") || !strcmp(cmd, "mode")) cmd_vidmode(argc, argv);
        else if (!strcmp(cmd, "lang")) cmd_lang(argc, argv);
        else if (!strcmp(cmd, "refresh") || !strcmp(cmd, "hz")) cmd_refresh(argc, argv);
        else if (!strcmp(cmd, "colors")) cmd_colors();
        else if (!strcmp(cmd, "beep")) beep(argc > 1 ? (u32)atoi(argv[1]) : 880, 200);
        else if (!strcmp(cmd, "alloc")) cmd_alloc(argc > 1 ? atoi(argv[1]) : 0);
        else if (!strcmp(cmd, "sleep")) {
            u32 ms = argc > 1 ? (u32)atoi(argv[1]) : 1000;
            kprintf(T("  sleeping for %u ms...\n", "  сплю %u мс...\n"), ms);
            task_sleep(ms);
            kputs(T("  awake\n\n", "  проснулся\n\n"));
        }
        else if (!strcmp(cmd, "history")) {
            kputc('\n');
            int start = hist_count > HIST_MAX ? hist_count - HIST_MAX : 0;
            for (int i = start; i < hist_count; i++)
                kprintf("   %3d  %s\n", i + 1, history[i % HIST_MAX]);
            kputc('\n');
        }
        else if (!strcmp(cmd, "crash")) {
            kputs(T("  triggering a division by zero...\n", "  провоцирую деление на ноль...\n"));
            volatile int a = 1, b = 0;
            volatile int c = a / b;
            (void)c;
        }
        else if (!strcmp(cmd, "reboot")) { kputs(T("  rebooting...\n", "  перезагрузка...\n")); sleep_ms(300); kv_reboot(); }
        else if (!strcmp(cmd, "poweroff") || !strcmp(cmd, "halt")) {
            kputs(T("  powering off...\n", "  выключение...\n")); sleep_ms(300); kv_poweroff();
        }
        else {
            vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
            kprintf(T("  kvsh: command '%s' not found. Type help.\n\n", "  kvsh: команда '%s' не найдена. Наберите help.\n\n"), cmd);
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        }
    }
}
