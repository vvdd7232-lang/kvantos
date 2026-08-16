/* ============================================================
 *  KvantOS - командная оболочка (kvsh)
 * ============================================================ */
#include "kernel.h"

#define CMD_MAX     200
#define ARGS_MAX    16
#define HIST_MAX    16

static char history[HIST_MAX][CMD_MAX];
static int  hist_count = 0;

/* Длина UTF-8 строки в символах (для выравнивания колонок) */
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

/* чтение строки с поддержкой Backspace и стрелок вверх/вниз */
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

/* ---------------- команды ---------------- */

/* Длина строки в знаках, а не байтах: кириллица в UTF-8 занимает
   два байта, и strlen дал бы вдвое больший отступ. */
static u32 ulen_ascii(const char *s) {
    u32 n = 0;
    while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; }
    return n;
}

/* ============================================================
 *  Работа с диском и установка приложений
 * ============================================================ */
/* Установка системы на винчестер. Спрашиваем подтверждение:
   операция переписывает загрузочный сектор диска. */
static void cmd_setup(int argc, char **argv) {
    kputs("\n");
    if (!setup_available()) {
        kputs("  Установка недоступна: система запущена не с установочного\n");
        kputs("  носителя, образ загрузчика не найден.\n\n");
        return;
    }
    if (!ata_present()) {
        kputs("  Жёсткий диск не найден.\n");
        kputs("  В VMware добавьте диск типа IDE, в QEMU - параметр -hda.\n\n");
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
    kputs("  УСТАНОВКА KvantOS НА ЖЁСТКИЙ ДИСК\n\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("    диск   : %s\n", ata_model(d));
    kprintf("    объём  : %u МиБ\n", ata_size_mb(d));
    kprintf("    файлы  : %s\n\n", keep ? "сохранить существующие" : "создать файловую систему заново");

    if (!confirmed) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kputs("  ВНИМАНИЕ: загрузочная запись диска будет перезаписана,\n");
        kputs("  а всё его содержимое станет недоступно.\n\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputs("  Если согласны, выполните:  setup --yes\n");
        kputs("  Сохранить файлы на диске:  setup --yes --keep\n\n");
        return;
    }

    kputs("  Установка началась...\n\n");
    int rc = setup_install(keep);
    kprintf("    %s\n", setup_stage_text());

    if (rc == 0) {
        vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        kprintf("\n  %s\n", setup_last_result());
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputs("  Извлеките носитель и наберите reboot.\n\n");
        beep(880, 80); beep(1320, 120);
    } else {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf("\n  Не удалось: %s\n\n", setup_last_result());
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }
}

static void cmd_disk(void) {
    kputs("\n");
    if (!ata_count()) {
        kputs("  Диски ATA не найдены.\n");
        kputs("  В QEMU добавьте: -hda disk.img\n\n");
        return;
    }
    kprintf("  Найдено дисков: %d\n\n", ata_count());
    for (int i = 0; i < ata_count(); i++) {
        kprintf("   [%d] %s\n", i, ata_model(i));
        kprintf("       секторов: %u, объём: %u МиБ%s\n",
                ata_sectors(i), ata_size_mb(i),
                i == ata_boot_drive() ? "  (системный)" : "");
    }
    kputs("\n");
}

static void cmd_format(void) {
    if (!ata_present()) { kputs("\n  Диск не найден.\n\n"); return; }
    kputs("\n  Разметка диска под KvFS...\n");
    int rc = kvfs_format();
    if (rc == 0) kputs("  Готово. Файловая система создана.\n\n");
    else kprintf("  Не удалось: %s\n\n", kvfs_error(rc));
}

static void cmd_df(void) {
    kputs("\n");
    if (!kvfs_mounted()) {
        kputs("  Файловая система не подключена.\n");
        kputs("  Разметьте диск командой format.\n\n");
        return;
    }
    u32 mb, kb, nf;
    kvfs_stats(&mb, &kb, &nf);
    kprintf("  KvFS на диске\n");
    kprintf("    объём диска : %u МиБ\n", mb);
    kprintf("    занято      : %u КиБ\n", kb);
    kprintf("    файлов      : %u из 64\n\n", nf);
}

static void cmd_dls(void) {
    kputs("\n");
    if (!kvfs_mounted()) { kputs("  Диск не размечен (format).\n\n"); return; }
    char nm[44]; u32 sz; int ex;
    int n = 0;
    kputs("   РАЗМЕР  ТИП   ИМЯ\n");
    for (int i = 0; i < 64; i++) {
        if (kvfs_list(i, nm, &sz, &ex) < 0) break;
        kprintf("  %7u  %s  %s\n", sz, ex ? "прог." : "файл ", nm);
        n++;
    }
    if (!n) kputs("  (пусто)\n");
    kprintf("\n  Файлов: %d\n\n", n);
}

static void cmd_dcat(const char *name) {
    if (!kvfs_mounted()) { kputs("\n  Диск не размечен.\n\n"); return; }
    u32 sz = kvfs_size(name);
    if (!sz) { kprintf("\n  Файл '%s' не найден.\n\n", name); return; }
    if (sz > 8192) sz = 8192;
    char *buf = (char *)kmalloc(sz + 1);
    if (!buf) { kputs("\n  Не хватает памяти.\n\n"); return; }
    int got = kvfs_read(name, buf, sz);
    if (got < 0) { kprintf("\n  Ошибка: %s\n\n", kvfs_error(got)); kfree(buf); return; }
    buf[got] = 0;
    kputs("\n");
    kputs(buf);
    kputs("\n\n");
    kfree(buf);
}

/* Установка: файл из ramfs переносится на диск. Признак «программа»
   ставится по расширению .kapp - именно такие файлы видит «Магазин». */
static void cmd_install(const char *name) {
    if (!kvfs_mounted()) {
        kputs("\n  Диск не размечен. Выполните format.\n\n");
        return;
    }
    rfile_t *f = ramfs_find(name);
    if (!f) {
        kprintf("\n  Файл '%s' не найден в ramfs.\n", name);
        kputs("  Список: ls\n\n");
        return;
    }
    /* .kapp в конце имени? */
    int is_app = 0;
    u32 l = (u32)strlen(name);
    if (l > 5 && !strcmp(name + l - 5, ".kapp")) is_app = 1;

    int rc = kvfs_write(name, f->data, f->size, is_app);
    if (rc == 0)
        kprintf("\n  Установлено: %s (%u байт)%s\n\n",
                name, f->size, is_app ? ", приложение" : "");
    else
        kprintf("\n  Не удалось: %s\n\n", kvfs_error(rc));
}

static void cmd_uninstall(const char *name) {
    if (!kvfs_mounted()) { kputs("\n  Диск не размечен.\n\n"); return; }
    int rc = kvfs_delete(name);
    if (rc == 0) kprintf("\n  Удалено: %s\n\n", name);
    else kprintf("\n  Не удалось: %s\n\n", kvfs_error(rc));
}

static void cmd_apps(void) {
    kputs("\n");
    if (!kvfs_mounted()) { kputs("  Диск не размечен (format).\n\n"); return; }
    char nm[44]; u32 sz; int ex;
    int n = 0;
    for (int i = 0; i < 64; i++) {
        if (kvfs_list(i, nm, &sz, &ex) < 0) break;
        if (!ex) continue;
        if (!n) kputs("  Установленные приложения:\n\n");
        /* kprintf не умеет левое выравнивание (%-24s), поэтому
           дополняем имя пробелами вручную. */
        kprintf("   %s", nm);
        for (int pad = (int)ulen_ascii(nm); pad < 24; pad++) kputc(' ');
        kprintf(" %u КиБ\n", (sz + 1023) / 1024);
        n++;
    }
    if (!n) {
        kputs("  Приложений нет.\n");
        kputs("  Соберите пример: sdk/make, затем install имя.kapp\n");
    }
    kputs("\n  Запуск: графический режим (guimenu), значок «Программы».\n\n");
}

static void cmd_help(void) {
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("\n  Команды KvantOS\n");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  ──────────────────────────────────────────────────────────\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    struct { const char *c, *d; } t[] = {
        {"help",      "показать эту справку"},
        {"about",     "сведения о системе и логотип"},
        {"clear",     "очистить экран (Ctrl+L)"},
        {"echo TEXT", "вывести текст"},
        {"mem",       "статистика памяти и кучи"},
        {"cpu",       "информация о процессоре"},
        {"uptime",    "время работы и тики таймера"},
        {"date",      "дата и время из CMOS/RTC"},
        {"ps",        "список задач планировщика"},
        {"spawn N",   "создать N фоновых задач-счётчиков"},
        {"ls",        "список файлов в ramfs"},
        {"cat FILE",  "показать содержимое файла"},
        {"write F T", "создать файл F с текстом T"},
        {"rm FILE",   "удалить файл"},
        {"guimenu",   "запустить графический режим (мышь + окна)"},
        {"setup",     "УСТАНОВИТЬ систему на жёсткий диск"},
        {"disk",      "сведения о дисках ATA"},
        {"format",    "разметить диск под KvFS (стирает данные!)"},
        {"df",        "занятость диска"},
        {"dls",       "файлы на диске"},
        {"dcat F",    "показать файл с диска"},
        {"install F", "поставить приложение из ramfs на диск"},
        {"uninstall F","удалить приложение с диска"},
        {"apps",      "список установленных приложений"},
        {"lspci [-v]","устройства на шине PCI"},
        {"gpu",       "сведения о видеокарте и драйвере"},
        {"vidmode",   "список и смена разрешения экрана"},
        {"refresh N", "частота обновления экрана в Гц"},
        {"gfx",       "сведения о видеорежиме и фреймбуфере"},
        {"hwreport",  "полный отчёт о железе (дубль в COM1)"},
        {"colors",    "палитра VGA (16 цветов)"},
        {"beep [Гц]", "звук через PC-спикер"},
        {"alloc N",   "выделить N байт в куче (тест)"},
        {"crash",     "вызвать исключение (тест паники)"},
        {"reboot",    "перезагрузить машину"},
        {"poweroff",  "выключить питание (ACPI)"},
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
    kprintf("   Архитектура : %s\n", KV_ARCH);
    kprintf("   Загрузчик   : GRUB 2 (Multiboot 1)\n");
    kprintf("   Ядро        : монолитное, собственной разработки\n");
    kprintf("   Сборка      : %s\n", KV_BUILD);
    kprintf("   Подсистемы  : GDT/IDT, PIC, PIT, PS/2, RTC, VGA, COM1,\n");
    kprintf("                 PMM, paging, куча, планировщик, ramfs\n\n");
}

static void cmd_mem(void) {
    u32 total = pmm_total_bytes();
    u32 used  = pmm_used_frames() * 4096;
    u32 ht, hu, hb;
    heap_stats(&ht, &hu, &hb);

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("  Физическая память\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("    всего   : %u КиБ (%u МиБ), страниц %u\n", total / 1024, total / 1048576, pmm_total_frames());
    kprintf("    занято  : %u КиБ (%u страниц)\n", used / 1024, pmm_used_frames());
    kprintf("    свободно: %u КиБ\n", (total - used) / 1024);

    /* полоска заполнения */
    u32 pct = total ? (used * 100u) / total : 0;
    kputs("    [");
    for (u32 i = 0; i < 40; i++) {
        if (i * 100u / 40u < pct) { vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK)); kputs("█"); }
        else { vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK)); kputs("░"); }
    }
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("] %u%%\n", pct);

    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("\n  Куча ядра\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("    размер  : %u КиБ\n", ht / 1024);
    kprintf("    занято  : %u байт\n", hu);
    kprintf("    блоков  : %u\n", hb);
    kprintf("    ядро    : %p .. %p\n\n", (void *)&kernel_start, (void *)&kernel_end);
}

static void cmd_cpu(void) {
    char vendor[16], brand[52];
    cpu_vendor(vendor);
    cpu_brand(brand);
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("  Процессор\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("    вендор : %s\n", vendor);
    kprintf("    модель : %s\n", brand);
    kprintf("    режим  : защищённый, 32 бита, страничная адресация вкл.\n\n");
}

static void cmd_uptime(void) {
    u32 s = timer_seconds();
    kprintf("\n  Время работы: %u ч %u мин %u с\n", s / 3600, (s / 60) % 60, s % 60);
    kprintf("  Тиков таймера: %u (частота %u Гц)\n\n", (u32)timer_ticks(), timer_hz());
}

static void cmd_date(void) {
    rtc_time_t t;
    rtc_read(&t);
    const char *mn[] = {"", "января", "февраля", "марта", "апреля", "мая", "июня",
                        "июля", "августа", "сентября", "октября", "ноября", "декабря"};
    kprintf("\n  %02u:%02u:%02u, %u %s %u года (UTC)\n\n",
            t.hour, t.min, t.sec, t.day,
            (t.month >= 1 && t.month <= 12) ? mn[t.month] : "?", t.year);
}

static void cmd_ps(void) {
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("   ID  ИМЯ              СОСТОЯНИЕ    ПЕРЕКЛЮЧЕНИЙ\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    task_t *cur = task_current();
    task_t *t = cur;
    int guard = 64;
    do {
        const char *st = t->state == TASK_READY ? "готова" :
                         t->state == TASK_SLEEPING ? "спит" : "завершена";
        if (t == cur) vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        kprintf("   %u", t->id);
        for (u32 s = t->id > 99 ? 3 : (t->id > 9 ? 2 : 1); s < 4; s++) kputc(' ');
        kprintf("%s", t->name);
        pad_to(t->name, 17);
        kprintf("%s", st);
        pad_to(st, 13);
        kprintf("%u%s\n", t->switches, t == cur ? "  <- текущая" : "");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        t = t->next;
    } while (t != cur && guard--);
    kprintf("\n  Всего активных задач: %u\n\n", task_count());
}

/* фоновая задача-счётчик: пишет точку в статус-строку через глобальный счётчик */
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
        if (t) kprintf("  создана задача #%u (%s)\n", t->id, t->name);
        else   kputs("  не удалось создать задачу\n");
    }
    kputc('\n');
}

static void cmd_ls(void) {
    rfile_t *tbl = ramfs_table();
    u32 count = 0, bytes = 0;
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("   РАЗМЕР  ИМЯ\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!tbl[i].used) continue;
        kprintf("   %6u  ", tbl[i].size);
        vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
        kprintf("%s\n", tbl[i].name);
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        count++; bytes += tbl[i].size;
    }
    kprintf("\n  Файлов: %u, суммарно %u байт (ramfs)\n\n", count, bytes);
}

static void cmd_cat(const char *name) {
    rfile_t *f = ramfs_find(name);
    if (!f) { kprintf("  cat: файл '%s' не найден\n\n", name); return; }
    kputc('\n');
    kputs(f->data);
    if (f->size && f->data[f->size - 1] != '\n') kputc('\n');
    kputc('\n');
}

static void cmd_colors(void) {
    const char *names[] = {"чёрный","синий","зелёный","бирюзовый","красный","пурпурный",
                           "коричневый","св.серый","т.серый","св.синий","св.зелёный",
                           "св.бирюз.","св.красный","розовый","жёлтый","белый"};
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

/* Сводный отчёт о железе: дублируется в COM1, чтобы его можно было
   снять с машины, у которой не видно экрана. */
static void cmd_hwreport(void) {
    char vend[16], brand[52];
    cpu_vendor(vend);
    cpu_brand(brand);

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("  Отчёт о конфигурации\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    kprintf("    ОС         : %s %s\n", KV_NAME, KV_VERSION);
    kprintf("    процессор  : %s\n", brand);
    kprintf("    вендор CPU : %s\n", vend);
    kprintf("    ОЗУ        : %u МиБ, страниц %u\n",
            pmm_total_bytes() / 1048576, pmm_total_frames());
    kprintf("    устройств PCI : %u\n", pci_count());

    pci_dev_t *g = pci_gpu();
    if (g)
        kprintf("    видеокарта : %s (%04x:%04x)\n",
                pci_gpu_model(g->vendor, g->device), g->vendor, g->device);
    else
        kputs("    видеокарта : не найдена на шине PCI\n");

    kprintf("    видеорежим : ");
    if (fb_active())
        kprintf("%u x %u, %u бит, шаг %u\n",
                fb_width(), fb_height(), fb_bpp_get(), fb_pitch_get());
    else
        kputs("текстовый VGA 80x25\n");

    kprintf("    интерфейс VBE : %s\n", vbe_backend_name());
    kprintf("    мышь PS/2  : %s\n", mouse_present() ? "есть" : "нет");

    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs("    отчёт продублирован в COM1 (38400 8N1)\n\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

static void cmd_gfx(void) {
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("  Видеоподсистема\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    if (!fb_active()) {
        kputs("    линейный фреймбуфер: недоступен\n");
        kputs("    активен текстовый режим VGA 80x25\n\n");
        return;
    }
    kprintf("    разрешение : %u x %u\n", fb_width(), fb_height());
    kprintf("    глубина    : %u бит на пиксель\n", fb_bpp_get());
    kprintf("    шаг строки : %u байт\n", fb_pitch_get());
    kprintf("    адрес      : %p (%u КиБ)\n", (void *)fb_base(), fb_bytes() / 1024);
    kprintf("    мышь PS/2  : %s\n", mouse_present() ? "обнаружена" : "нет");
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs("    наберите guimenu для запуска графической среды\n\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

static void cmd_guimenu(void) {
    if (!fb_active()) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kputs("\n  Графический режим недоступен: загрузчик не предоставил\n");
        kputs("  линейный фреймбуфер. Выберите обычный пункт меню GRUB.\n\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }
    kputs("\n  Переключение в графический режим...\n");
    kputs("  Выход обратно в консоль: Esc или Q\n");
    sleep_ms(400);

    gui_run();

    /* возврат в текстовый режим */
    vga_text_mode_restore();
    vga_clear();
    logo_print();
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs("\n  Возврат в текстовую консоль kvsh.\n\n");
}

static void cmd_alloc(int n) {
    if (n <= 0) { kputs("  использование: alloc <байт>\n\n"); return; }
    u32 ht, hu, hb;
    heap_stats(&ht, &hu, &hb);
    if ((u32)n > ht) {
        kprintf("  запрошено больше размера кучи (%u КиБ)\n\n", ht / 1024);
        return;
    }
    void *p = kmalloc((size_t)n);
    if (!p) { kputs("  kmalloc вернул NULL (не хватило памяти)\n\n"); return; }
    memset(p, 0xAA, (size_t)n);
    kprintf("  выделено %d байт по адресу %p (физ. %p)\n", n, p, (void *)paging_phys((u32)p));
    kfree(p);
    kputs("  блок освобождён\n\n");
}

/* ---------------- главный цикл ---------------- */

static void status_task(void) {
    for (;;) {
        char right[64];
        u32 s = timer_seconds();
        u32 total = pmm_total_bytes() / 1048576;
        u32 used  = pmm_used_frames() * 4096 / 1048576;
        ksnprintf(right, sizeof(right), "ОЗУ %u/%u МиБ | задач %u | %02u:%02u:%02u",
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
    kprintf("\n   Добро пожаловать в %s %s!\n", KV_NAME, KV_VERSION);
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("   Наберите ");
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs("help");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs(" для списка команд, ");
    vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    kputs("guimenu");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs(" — графический режим.\n\n");
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

        /* Имя команды приводим к нижнему регистру: при включённом
           CapsLock иначе получалось "MEM" - команда не найдена.
           Аргументы (имена файлов, текст) НЕ трогаем. */
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
            if (argc < 2) kputs("\n  Использование: dcat ИМЯ\n\n");
            else cmd_dcat(argv[1]);
        }
        else if (!strcmp(cmd, "install")) {
            if (argc < 2) kputs("\n  Использование: install ИМЯ\n\n");
            else cmd_install(argv[1]);
        }
        else if (!strcmp(cmd, "uninstall")) {
            if (argc < 2) kputs("\n  Использование: uninstall ИМЯ\n\n");
            else cmd_uninstall(argv[1]);
        }
        else if (!strcmp(cmd, "apps")) cmd_apps();
        else if (!strcmp(cmd, "mem") || !strcmp(cmd, "free")) cmd_mem();
        else if (!strcmp(cmd, "cpu")) cmd_cpu();
        else if (!strcmp(cmd, "uptime")) cmd_uptime();
        else if (!strcmp(cmd, "date") || !strcmp(cmd, "time")) cmd_date();
        else if (!strcmp(cmd, "ps") || !strcmp(cmd, "tasks")) cmd_ps();
        else if (!strcmp(cmd, "spawn")) cmd_spawn(argc > 1 ? atoi(argv[1]) : 1);
        else if (!strcmp(cmd, "ls") || !strcmp(cmd, "dir")) cmd_ls();
        else if (!strcmp(cmd, "cat")) {
            if (argc < 2) kputs("  использование: cat <файл>\n\n");
            else cmd_cat(argv[1]);
        }
        else if (!strcmp(cmd, "write")) {
            if (argc < 3) kputs("  использование: write <файл> <текст...>\n\n");
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
                if (r == 0) kprintf("  создан файл '%s' (%u байт)\n\n", argv[1], pos);
                else if (r == -2) kprintf("  файл '%s' уже существует\n\n", argv[1]);
                else if (r == -5) kputs("  имя файла слишком длинное (максимум 23 символа)\n\n");
                else if (r == -4) kputs("  пустое имя файла\n\n");
                else if (r == -3) kputs("  не хватает памяти в куче\n\n");
                else kputs("  ramfs переполнена\n\n");
            }
        }
        else if (!strcmp(cmd, "rm") || !strcmp(cmd, "del")) {
            if (argc < 2) kputs("  использование: rm <файл>\n\n");
            else if (ramfs_delete(argv[1]) == 0) kprintf("  файл '%s' удалён\n\n", argv[1]);
            else kprintf("  rm: '%s' не найден\n\n", argv[1]);
        }
        else if (!strcmp(cmd, "guimenu") || !strcmp(cmd, "gui") || !strcmp(cmd, "startx")) cmd_guimenu();
        else if (!strcmp(cmd, "gfx") || !strcmp(cmd, "video")) cmd_gfx();
        else if (!strcmp(cmd, "hwreport") || !strcmp(cmd, "hw")) cmd_hwreport();
        else if (!strcmp(cmd, "lspci")) cmd_lspci(argc > 1 && !strcmp(argv[1], "-v"));
        else if (!strcmp(cmd, "gpu") || !strcmp(cmd, "gpuinfo")) cmd_gpuinfo();
        else if (!strcmp(cmd, "vidmode") || !strcmp(cmd, "mode")) cmd_vidmode(argc, argv);
        else if (!strcmp(cmd, "refresh") || !strcmp(cmd, "hz")) cmd_refresh(argc, argv);
        else if (!strcmp(cmd, "colors")) cmd_colors();
        else if (!strcmp(cmd, "beep")) beep(argc > 1 ? (u32)atoi(argv[1]) : 880, 200);
        else if (!strcmp(cmd, "alloc")) cmd_alloc(argc > 1 ? atoi(argv[1]) : 0);
        else if (!strcmp(cmd, "sleep")) {
            u32 ms = argc > 1 ? (u32)atoi(argv[1]) : 1000;
            kprintf("  сплю %u мс...\n", ms);
            task_sleep(ms);
            kputs("  проснулся\n\n");
        }
        else if (!strcmp(cmd, "history")) {
            kputc('\n');
            int start = hist_count > HIST_MAX ? hist_count - HIST_MAX : 0;
            for (int i = start; i < hist_count; i++)
                kprintf("   %3d  %s\n", i + 1, history[i % HIST_MAX]);
            kputc('\n');
        }
        else if (!strcmp(cmd, "crash")) {
            kputs("  провоцирую деление на ноль...\n");
            volatile int a = 1, b = 0;
            volatile int c = a / b;
            (void)c;
        }
        else if (!strcmp(cmd, "reboot")) { kputs("  перезагрузка...\n"); sleep_ms(300); kv_reboot(); }
        else if (!strcmp(cmd, "poweroff") || !strcmp(cmd, "halt")) {
            kputs("  выключение...\n"); sleep_ms(300); kv_poweroff();
        }
        else {
            vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
            kprintf("  kvsh: команда '%s' не найдена. Наберите help.\n\n", cmd);
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        }
    }
}
