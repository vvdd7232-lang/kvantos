/* ============================================================
 *  KvantOS - extra shell commands (kvsh utilities)
 *
 *  Everything here is a plain shell command: it reads its
 *  arguments, prints to the console and returns. The dispatch
 *  lives in shell.c, the prototypes in kernel.h.
 * ============================================================ */
#include "kernel.h"

/* ---------- small helpers ---------- */

/* Parse a decimal/hex/binary number ("42", "-7", "0x1f", "0b101").
   Returns 1 on success and stores the value in *out. */
static int parse_num(const char *s, int *out) {
    if (!s || !*s) return 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (!*s) return 0;

    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
    if (!*s) return 0;

    u32 v = 0;
    while (*s) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        if (d >= base) return 0;
        v = v * (u32)base + (u32)d;
        s++;
    }
    *out = neg ? -(int)v : (int)v;
    return 1;
}

/* A byte-wise substring search (the kernel string kit has none) */
static const char *k_strstr(const char *hay, const char *needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return NULL;
}

/* ============================================================
 *  System information
 * ============================================================ */

void cmd_uname(void) {
    kprintf("%s %s %s (build %s)\n", KV_NAME, KV_VERSION, KV_ARCH, KV_BUILD);
}

void cmd_hostname(void) {
    kprintf("\n  %s\n\n", "kvantos");
}

void cmd_whoami(void) {
    kputs(T("\n  You are root: KvantOS has no user accounts yet -\n"
            "  everyone who reaches the shell is the operator.\n\n",
            "\n  Вы root: пользователей в KvantOS пока нет -\n"
            "  каждый, кто добрался до оболочки, считается оператором.\n\n"));
}

/* A one-screen summary of everything the kernel knows */
void cmd_sysinfo(void) {
    char vendor[16], brand[52];
    cpu_vendor(vendor);
    cpu_brand(brand);

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  System summary\n", "  Сводка о системе\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    kprintf(T("    OS        : %s %s\n", "    ОС        : %s %s\n"), KV_NAME, KV_VERSION);
    kprintf(T("    CPU       : %s\n", "    ЦП        : %s\n"), brand);
    kprintf(T("    RAM       : %u MiB total, %u MiB used\n",
              "    ОЗУ       : всего %u МиБ, занято %u МиБ\n"),
            pmm_total_bytes() / 1048576, pmm_used_frames() * 4096 / 1048576);
    kprintf(T("    tasks     : %u in the scheduler\n",
              "    задач     : %u у планировщика\n"), task_count());

    u32 s = timer_seconds();
    kprintf(T("    uptime    : %uh %02um %02us\n", "    аптайм    : %u ч %02u мин %02u с\n"),
            s / 3600, (s / 60) % 60, s % 60);

    if (fb_active())
        kprintf(T("    screen    : %ux%u, %u bpp (graphics)\n",
                  "    экран     : %ux%u, %u бит (графика)\n"),
                fb_width(), fb_height(), fb_bpp_get());
    else
        kputs(T("    screen    : 80x25 text mode\n", "    экран     : текстовый режим 80x25\n"));

    kprintf(T("    PCI       : %u devices\n", "    PCI       : устройств %u\n"), pci_count());
    kprintf(T("    ATA disks : %u\n", "    диски ATA : %u\n"), ata_count());
    kprintf(T("    language  : %s\n", "    язык      : %s\n"), kv_lang_name());
    kputc('\n');
}

/* ============================================================
 *  Calculator: integer arithmetic with parentheses
 *  calc 2+2*2, calc "(1+2)*3", spaces are fine
 * ============================================================ */

static const char *calc_p;
static int calc_err;

static int calc_expr(void);

static void calc_skip_ws(void) {
    while (*calc_p == ' ') calc_p++;
}

static int calc_factor(void) {
    calc_skip_ws();
    if (*calc_p == '(') {
        calc_p++;
        int v = calc_expr();
        calc_skip_ws();
        if (*calc_p != ')') { calc_err = 1; return 0; }
        calc_p++;
        return v;
    }
    if (*calc_p == '-') { calc_p++; return -calc_factor(); }
    if (*calc_p == '+') { calc_p++; return calc_factor(); }

    if (*calc_p < '0' || *calc_p > '9') { calc_err = 1; return 0; }
    int v = 0;
    while (*calc_p >= '0' && *calc_p <= '9') {
        v = v * 10 + (*calc_p - '0');
        calc_p++;
    }
    return v;
}

static int calc_term(void) {
    int v = calc_factor();
    for (;;) {
        calc_skip_ws();
        char op = *calc_p;
        if (op != '*' && op != '/' && op != '%') return v;
        calc_p++;
        int r = calc_factor();
        if ((op == '/' || op == '%') && r == 0) { calc_err = 2; return 0; }
        if (op == '*') v = v * r;
        else if (op == '/') v = v / r;
        else v = v % r;
    }
}

static int calc_expr(void) {
    int v = calc_term();
    for (;;) {
        calc_skip_ws();
        char op = *calc_p;
        if (op != '+' && op != '-') return v;
        calc_p++;
        int r = calc_term();
        if (op == '+') v = v + r;
        else v = v - r;
    }
}

void cmd_calc(int argc, char **argv) {
    if (argc < 2) {
        kputs(T("\n  Usage: calc EXPRESSION\n", "\n  Использование: calc ВЫРАЖЕНИЕ\n"));
        kputs(T("  Example: calc (2+3)*4  ->  20\n\n", "  Пример: calc (2+3)*4  ->  20\n\n"));
        return;
    }
    /* join all arguments: "calc 2 + 2" is as valid as "calc 2+2" */
    char buf[160];
    u32 pos = 0;
    for (int i = 1; i < argc && pos < sizeof(buf) - 2; i++) {
        for (const char *s = argv[i]; *s && pos < sizeof(buf) - 2; s++)
            buf[pos++] = *s;
        if (i + 1 < argc) buf[pos++] = ' ';
    }
    buf[pos] = 0;

    calc_p = buf;
    calc_err = 0;
    int result = calc_expr();
    calc_skip_ws();
    if (calc_err == 2) kputs(T("  calc: division by zero\n\n", "  calc: деление на ноль\n\n"));
    else if (calc_err || *calc_p) kputs(T("  calc: bad expression\n\n", "  calc: ошибка в выражении\n\n"));
    else kprintf("  %s = %d\n\n", buf, result);
}

/* ============================================================
 *  Number base conversions: hex, bin, dec
 * ============================================================ */

static int need_num_arg(const char *cmd, const char *arg) {
    if (!arg) {
        kprintf(T("\n  Usage: %s NUMBER (0x.. and 0b.. are understood)\n\n",
                  "\n  Использование: %s ЧИСЛО (понимает 0x.. и 0b..)\n\n"), cmd);
        return 0;
    }
    return 1;
}

void cmd_hex(const char *arg) {
    int v;
    if (!need_num_arg("hex", arg)) return;
    if (!parse_num(arg, &v)) { kputs(T("  hex: not a number\n\n", "  hex: это не число\n\n")); return; }
    kprintf("  %d = 0x%x\n\n", v, (u32)v);
}

void cmd_bin(const char *arg) {
    int v;
    if (!need_num_arg("bin", arg)) return;
    if (!parse_num(arg, &v)) { kputs(T("  bin: not a number\n\n", "  bin: это не число\n\n")); return; }
    kprintf("  %d = 0b%b\n\n", v, (u32)v);
}

void cmd_dec(const char *arg) {
    int v;
    if (!need_num_arg("dec", arg)) return;
    if (!parse_num(arg, &v)) { kputs(T("  dec: not a number\n\n", "  dec: это не число\n\n")); return; }
    kprintf("  %s = %d\n\n", arg, v);
}

/* ============================================================
 *  Calendar
 * ============================================================ */

/* 0 = Sunday ... 6 = Saturday (proleptic Gregorian calendar) */
static int day_of_week(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int days_in_month(int y, int m) {
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

void cmd_cal(int argc, char **argv) {
    rtc_time_t now;
    rtc_read(&now);
    int month = now.month, year = now.year;
    if (argc >= 2 && !parse_num(argv[1], &month)) {
        kputs(T("  cal: month must be a number 1-12\n\n", "  cal: месяц должен быть числом 1-12\n\n"));
        return;
    }
    if (argc >= 3 && !parse_num(argv[2], &year)) {
        kputs(T("  cal: bad year\n\n", "  cal: неверный год\n\n"));
        return;
    }
    if (month < 1 || month > 12) {
        kputs(T("  cal: month must be 1-12\n\n", "  cal: месяц должен быть 1-12\n\n"));
        return;
    }

    static const char *mn_en[] = {"", "January", "February", "March", "April", "May", "June",
                                  "July", "August", "September", "October", "November", "December"};
    static const char *mn_ru[] = {"", "Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
                                  "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"};

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kprintf("     %s %d\n", kv_pick(mn_en[month], mn_ru[month]), year);
    vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    kputs(T("  Mo Tu We Th Fr Sa Su\n", "  Пн Вт Ср Чт Пт Сб Вс\n"));

    int first = (day_of_week(year, month, 1) + 6) % 7;   /* Monday = 0 */
    int days = days_in_month(year, month);
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs("  ");
    for (int i = 0; i < first; i++) kputs("   ");
    for (int d = 1; d <= days; d++) {
        int wd = (first + d - 1) % 7;
        if (d == now.day && month == (int)now.month && year == (int)now.year)
            vga_set_color(VGA_COLOR(VGA_WHITE, VGA_BLUE));         /* today */
        else if (wd >= 5)
            vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));         /* weekend */
        else
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kprintf(" %2d", d);
        if (wd == 6 && d != days) kputs("\n  ");
    }
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs("\n\n");
}

/* ============================================================
 *  Random numbers and sequences
 * ============================================================ */

void cmd_rand(int argc, char **argv) {
    u32 n = kv_rand();
    if (argc > 1) {
        int max;
        if (!parse_num(argv[1], &max) || max <= 0) {
            kputs(T("  rand: upper bound must be a positive number\n\n",
                    "  rand: верхняя граница должна быть положительным числом\n\n"));
            return;
        }
        n = kv_rand_max((u32)max);
    }
    kprintf("\n  %u\n\n", n);
}

void cmd_seq(int argc, char **argv) {
    int a, b;
    if (argc < 3 || !parse_num(argv[1], &a) || !parse_num(argv[2], &b)) {
        kputs(T("\n  Usage: seq FROM TO\n\n", "\n  Использование: seq ОТ ДО\n\n"));
        return;
    }
    if (b < a) { int t = a; a = b; b = t; }
    if (b - a > 10000) b = a + 10000;      /* keep the flood bounded */
    kputc('\n');
    for (int i = a; i <= b; i++) kprintf("  %d\n", i);
    kputc('\n');
}

/* Reverse the text byte by byte (a toy, not UTF-8 aware) */
void cmd_rev(int argc, char **argv) {
    if (argc < 2) { kputs(T("\n  Usage: rev TEXT\n\n", "\n  Использование: rev ТЕКСТ\n\n")); return; }
    char buf[180];
    u32 pos = 0;
    for (int i = 1; i < argc && pos < sizeof(buf) - 2; i++) {
        for (const char *s = argv[i]; *s && pos < sizeof(buf) - 2; s++)
            buf[pos++] = *s;
        if (i + 1 < argc) buf[pos++] = ' ';
    }
    buf[pos] = 0;
    kputc('\n');
    for (int i = (int)pos - 1; i >= 0; i--) kputc(buf[i]);
    kputs("\n\n");
}

/* ============================================================
 *  File tools for ramfs: wc, grep, hexdump, sum, cp, mv, touch
 * ============================================================ */

static rfile_t *file_or_warn(const char *cmd, const char *name) {
    rfile_t *f = ramfs_find(name);
    if (!f) kprintf(T("  %s: file '%s' not found\n\n", "  %s: файл '%s' не найден\n\n"), cmd, name);
    return f;
}

void cmd_wc(const char *name) {
    if (!name) { kputs(T("\n  Usage: wc FILE\n\n", "\n  Использование: wc ФАЙЛ\n\n")); return; }
    rfile_t *f = file_or_warn("wc", name);
    if (!f) return;
    u32 lines = 0, words = 0, in_word = 0;
    for (u32 i = 0; i < f->size; i++) {
        char c = f->data[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    kprintf("\n  %s: %u %s, %u %s, %u %s\n\n", name, lines, T("lines", "строк"),
            words, T("words", "слов"), f->size, T("bytes", "байт"));
}

void cmd_grep(int argc, char **argv) {
    if (argc < 3) {
        kputs(T("\n  Usage: grep NEEDLE FILE\n\n", "\n  Использование: grep СТРОКА ФАЙЛ\n\n"));
        return;
    }
    rfile_t *f = file_or_warn("grep", argv[2]);
    if (!f) return;
    const char *needle = argv[1];
    u32 hits = 0, i = 0;
    kputc('\n');
    while (i < f->size) {
        u32 start = i;
        while (i < f->size && f->data[i] != '\n') i++;
        /* search inside data[start..i) */
        u32 len = i - start;
        char line[200];
        u32 n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, f->data + start, n);
        line[n] = 0;
        if (k_strstr(line, needle)) {
            kprintf("  %s\n", line);
            hits++;
        }
        if (i < f->size) i++;               /* skip the newline */
    }
    kprintf(T("  matches: %u\n\n", "  совпадений: %u\n\n"), hits);
}

void cmd_hexdump(const char *name) {
    if (!name) { kputs(T("\n  Usage: hexdump FILE\n\n", "\n  Использование: hexdump ФАЙЛ\n\n")); return; }
    rfile_t *f = file_or_warn("hexdump", name);
    if (!f) return;
    kputc('\n');
    for (u32 off = 0; off < f->size; off += 16) {
        kprintf("  %04x  ", off);
        for (u32 j = 0; j < 16; j++) {
            if (off + j < f->size) kprintf("%02x ", (u32)(u8)f->data[off + j]);
            else kputs("   ");
            if (j == 7) kputc(' ');
        }
        kputs(" |");
        for (u32 j = 0; j < 16 && off + j < f->size; j++) {
            char c = f->data[off + j];
            kputc((c >= 32 && c < 127) ? c : '.');
        }
        kputs("|\n");
    }
    kprintf(T("  %u bytes\n\n", "  %u байт\n\n"), f->size);
}

/* FNV-1a checksum: quick way to see whether a file changed */
void cmd_sum(const char *name) {
    if (!name) { kputs(T("\n  Usage: sum FILE\n\n", "\n  Использование: sum ФАЙЛ\n\n")); return; }
    rfile_t *f = file_or_warn("sum", name);
    if (!f) return;
    u32 h = 2166136261u;
    for (u32 i = 0; i < f->size; i++) {
        h ^= (u8)f->data[i];
        h *= 16777619u;
    }
    kprintf("\n  %s: FNV-1a %x (%u bytes)\n\n", name, h, f->size);
}

void cmd_touch(const char *name) {
    if (!name) { kputs(T("\n  Usage: touch FILE\n\n", "\n  Использование: touch ФАЙЛ\n\n")); return; }
    int r = ramfs_create(name, "", 0);
    if (r == 0) kprintf(T("  file '%s' created (empty)\n\n", "  создан файл '%s' (пустой)\n\n"), name);
    else if (r == -2) kprintf(T("  file '%s' already exists\n\n", "  файл '%s' уже существует\n\n"), name);
    else kputs(T("  touch: ramfs is full or the name is bad\n\n", "  touch: ramfs переполнена или имя неверно\n\n"));
}

void cmd_cp(int argc, char **argv) {
    if (argc < 3) { kputs(T("\n  Usage: cp SRC DST\n\n", "\n  Использование: cp ОТКУДА КУДА\n\n")); return; }
    rfile_t *src = file_or_warn("cp", argv[1]);
    if (!src) return;
    if (ramfs_find(argv[2])) {
        if (ramfs_delete(argv[2]) != 0) {
            kputs(T("  cp: cannot remove the destination\n\n", "  cp: не удалить файл назначения\n\n"));
            return;
        }
    }
    if (ramfs_create(argv[2], src->data, src->size) == 0)
        kprintf(T("  '%s' -> '%s' (%u bytes)\n\n", "  '%s' -> '%s' (%u байт)\n\n"), argv[1], argv[2], src->size);
    else
        kputs(T("  cp: ramfs is full or the name is bad\n\n", "  cp: ramfs переполнена или имя неверно\n\n"));
}

void cmd_mv(int argc, char **argv) {
    if (argc < 3) { kputs(T("\n  Usage: mv SRC DST\n\n", "\n  Использование: mv ОТКУДА КУДА\n\n")); return; }
    rfile_t *src = file_or_warn("mv", argv[1]);
    if (!src) return;
    if (strcmp(argv[1], argv[2]) == 0) { kputs(T("  mv: source and destination are the same\n\n", "  mv: источник и назначение совпадают\n\n")); return; }
    if (ramfs_find(argv[2])) ramfs_delete(argv[2]);
    char name[24];
    strncpy(name, src->name, sizeof(name));
    char *data = src->data;
    u32 size = src->size;
    if (ramfs_create(argv[2], data, size) != 0) {
        kputs(T("  mv: ramfs is full or the name is bad\n\n", "  mv: ramfs переполнена или имя неверно\n\n"));
        return;
    }
    ramfs_delete(name);
    kprintf(T("  '%s' renamed to '%s'\n\n", "  '%s' переименован в '%s'\n\n"), name, argv[2]);
}

/* ============================================================
 *  Tasks: kill
 * ============================================================ */

void cmd_kill(int argc, char **argv) {
    if (argc < 2) {
        kputs(T("\n  Usage: kill ID | NAME   (see ps for the list)\n\n",
                "\n  Использование: kill ID | ИМЯ   (список даёт ps)\n\n"));
        return;
    }
    u32 target_id = 0;
    int by_id = str_isnum(argv[1]);
    if (by_id) target_id = (u32)atoi(argv[1]);

    task_t *cur = task_current();
    task_t *t = cur;
    int guard = 256;
    task_t *victim = NULL;
    do {
        if (t->state != TASK_DEAD) {
            if ((by_id && t->id == target_id) || (!by_id && strcmp(t->name, argv[1]) == 0)) {
                victim = t;
                break;
            }
        }
        t = t->next;
    } while (t != cur && --guard > 0);

    if (!victim) {
        kprintf(T("  kill: no such task: '%s'\n\n", "  kill: такой задачи нет: '%s'\n\n"), argv[1]);
        return;
    }
    if (!strcmp(victim->name, "kernel")) {
        kputs(T("  kill: refusing to kill the kernel - that is a reboot with extra steps\n\n",
                "  kill: ядро убивать отказываюсь - это та же перезагрузка, только сложнее\n\n"));
        return;
    }
    int r = task_kill(victim->id);
    if (r == 0) kprintf(T("  task #%u (%s) terminated\n\n", "  задача #%u (%s) завершена\n\n"), victim->id, victim->name);
    else if (r == -2) kputs(T("  kill: cannot terminate the current task\n\n", "  kill: текущую задачу завершить нельзя\n\n"));
    else kprintf(T("  kill: task #%u not found\n\n", "  kill: задача #%u не найдена\n\n"), victim->id);
}

/* ============================================================
 *  Fun and effects: matrix, fortune, melody, leds, ascii
 * ============================================================ */

/* Text-mode digital rain. Any key stops it. */
void cmd_matrix(void) {
    kputs(T("  The Matrix. Press any key to wake up.\n", "  Матрица. Нажмите любую клавишу, чтобы проснуться.\n"));
    static const char glyphs[] = "01<>[]{}#$%*+=|/\\~^;:abcdef0123456789";
    for (int frame = 0; frame < 160; frame++) {
        if (kbd_getchar_nb() >= 0) break;
        for (int col = 0; col < 76; col++) {
            u32 r = kv_rand();
            if ((r & 7) < 3) { kputc(' '); continue; }   /* mostly empty */
            char c = glyphs[r % (sizeof(glyphs) - 1)];
            u32 tone = (r >> 3) & 15;
            if (tone == 0) vga_set_color(VGA_COLOR(VGA_WHITE, VGA_BLACK));
            else if (tone < 6) vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
            else vga_set_color(VGA_COLOR(VGA_GREEN, VGA_BLACK));
            kputc(c);
        }
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputc('\n');
        task_sleep(30);
    }
    kputc('\n');
}

void cmd_fortune(void) {
    static const char *q_en[] = {
        "Any sufficiently advanced technology is indistinguishable from magic. - A. Clarke",
        "Talk is cheap. Show me the code. - L. Torvalds",
        "Simple is better than complex. - T. Peters",
        "Programs must be written for people to read. - H. Abelson",
        "There is no place like 127.0.0.1.",
        "It works on my machine.",
        "The best way to predict the future is to invent it. - A. Kay",
        "First, solve the problem. Then, write the code. - J. Johnson",
        "A kernel is just a program that never gets to exit.",
        "Real programmers count from 0.",
    };
    static const char *q_ru[] = {
        "Достаточно развитая технология неотличима от магии. - А. Кларк",
        "Разговоры дешевы. Покажи мне код. - Л. Торвальдс",
        "Простое лучше сложного. - Т. Питерс",
        "Программы пишутся для людей, а читаются лишь попутно машинами. - Х. Абельсон",
        "Нет места лучше, чем 127.0.0.1.",
        "У меня на машине всё работает.",
        "Лучший способ предсказать будущее - изобрести его. - А. Кей",
        "Сначала реши задачу, а уже потом пиши код. - Дж. Джонсон",
        "Ядро - это программа, которой никогда не дают завершиться.",
        "Настоящие программисты считают от нуля.",
    };
    enum { NQ = 10 };
    u32 i = kv_rand_max(NQ);
    kprintf("\n  %s\n\n", kv_pick(q_en[i], q_ru[i]));
}

/* The opening phrase of Fur Elise on the PC speaker */
void cmd_melody(void) {
    kputs(T("  Playing on the PC speaker...\n", "  Играю на PC-спикере...\n"));
    static const struct { u16 hz, ms; } tune[] = {
        {659, 200}, {622, 200}, {659, 200}, {622, 200}, {659, 200},
        {494, 200}, {523, 200}, {659, 200}, {440, 400},
        {0,   100}, {262, 200}, {330, 200}, {440, 400},
    };
    for (u32 i = 0; i < sizeof(tune) / sizeof(tune[0]); i++) {
        if (tune[i].hz) beep(tune[i].hz, tune[i].ms);
        task_sleep(tune[i].ms + 30);
        if (kbd_getchar_nb() >= 0) break;     /* a key ends the concert */
    }
    kputc('\n');
}

void cmd_leds(int argc, char **argv) {
    if (argc < 2) {
        kputs(T("\n  Usage: leds on | off | scroll | num | caps | dance\n\n",
                "\n  Использование: leds on | off | scroll | num | caps | dance\n\n"));
        return;
    }
    const char *a = argv[1];
    if (!strcmp(a, "on"))       kbd_set_leds(0x07);
    else if (!strcmp(a, "off")) kbd_set_leds(0x00);
    else if (!strcmp(a, "scroll")) kbd_set_leds(0x01);
    else if (!strcmp(a, "num"))    kbd_set_leds(0x02);
    else if (!strcmp(a, "caps"))   kbd_set_leds(0x04);
    else if (!strcmp(a, "dance")) {
        u8 save = kbd_get_leds();
        static const u8 steps[] = {1, 2, 4, 3, 5, 6, 7, 0};
        for (u32 i = 0; i < sizeof(steps); i++) {
            kbd_set_leds(steps[i]);
            task_sleep(220);
        }
        kbd_set_leds(save);
    }
    else kputs(T("  leds: unknown mode\n\n", "  leds: неизвестный режим\n\n"));
}

/* The printable part of the ASCII table */
void cmd_ascii(void) {
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  ASCII table (codes 32-126)\n", "  Таблица ASCII (коды 32-126)\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            int c = 32 + row + col * 12;
            if (c > 126) break;
            kprintf("  %3d=%c", c, c);
        }
        kputc('\n');
    }
    kputs(T("\n  Ctrl+C = 3, Ctrl+L = 12 (clear), Backspace = 8\n\n",
            "\n  Ctrl+C = 3, Ctrl+L = 12 (очистка), Backspace = 8\n\n"));
}

/* ============================================================
 *  Countdown timer
 * ============================================================ */

void cmd_countdown(int argc, char **argv) {
    int secs = 5;
    if (argc > 1 && (!parse_num(argv[1], &secs) || secs < 1 || secs > 3600)) {
        kputs(T("  countdown: give me 1..3600 seconds\n\n", "  countdown: укажите 1..3600 секунд\n\n"));
        return;
    }
    for (int i = secs; i > 0; i--) {
        kprintf(T("  %d...\n", "  %d...\n"), i);
        if (i <= 3) beep(660, 80);
        task_sleep(1000);
    }
    beep(1320, 400);
    kputs(T("  Time is up!\n\n", "  Время вышло!\n\n"));
}

/* ============================================================
 *  Shell text colour
 * ============================================================ */


void cmd_color(int argc, char **argv) {
    static const char *names_en[] = {"black","blue","green","cyan","red","magenta","brown","ltgray",
                                     "dkgray","ltblue","ltgreen","ltcyan","ltred","pink","yellow","white"};
    if (argc < 2 || !strcmp(argv[1], "help")) {
        kputs(T("\n  Usage: color FG [BG]   (numbers 0-15, see 'colors')\n",
                "\n  Использование: color ЦВЕТ [ФОН]   (числа 0-15, см. 'colors')\n"));
        kputs(T("         color reset     (back to light gray)\n\n",
                "         color reset     (вернуть светло-серый)\n\n"));
        for (int i = 0; i < 16; i++) kprintf("   %2d %s\n", i, names_en[i]);
        kputc('\n');
        return;
    }
    if (!strcmp(argv[1], "reset")) {
        shell_set_fg(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputs(T("  colour reset\n\n", "  цвет сброшен\n\n"));
        return;
    }
    int fg, bg = VGA_BLACK;
    if (!parse_num(argv[1], &fg) || fg < 0 || fg > 15 ||
        (argc > 2 && (!parse_num(argv[2], &bg) || bg < 0 || bg > 15))) {
        kputs(T("  color: numbers 0-15, please\n\n", "  color: нужны числа 0-15\n\n"));
        return;
    }
    shell_set_fg(VGA_COLOR((u8)fg, (u8)bg));
    kputs(T("  shell colour changed\n\n", "  цвет оболочки изменён\n\n"));
}
