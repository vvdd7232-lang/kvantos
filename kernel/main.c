/* ============================================================
 *  KvantOS - точка входа ядра
 * ============================================================ */
#include "kernel.h"
#include "kvapp.h"

#define MULTIBOOT_MAGIC 0x2BADB002

#define HEAP_BASE 0x00400000      /* 4 MiB */
#define HEAP_SIZE 0x00A00000      /* 10 MiB */

static void step(const char *what) {
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  [");
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs("ok");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("] ");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs(what);
    kputc('\n');
}

static void idle_task(void) {
    for (;;) { __asm__ volatile("sti; hlt"); }
}

void kmain(u32 magic, u32 mbi_addr) {
    multiboot_info_t *mbi = (multiboot_info_t *)mbi_addr;

    serial_init();

    /* Загрузчик может передать в командной строке слово text -
       тогда принудительно работаем в классическом VGA 80x25. */
    int force_text = 0, safe_mode = 0;
    if ((mbi->flags & (1u << 2)) && mbi->cmdline) {
        const char *p = (const char *)mbi->cmdline;
        for (; *p; p++) {
            if (p[0] == 't' && p[1] == 'e' && p[2] == 'x' && p[3] == 't') force_text = 1;
            if (p[0] == 's' && p[1] == 'a' && p[2] == 'f' && p[3] == 'e') safe_mode = 1;
        }
    }
    if (safe_mode) force_text = 1;   /* безопасный режим всегда текстовый */

    /* GRUB уже включил графику по запросу из Multiboot-заголовка,
       поэтому для текстового режима возвращаем адаптер обратно сами. */
    if (force_text) vbe_force_text();

    int have_fb = force_text ? 0 : fb_init(mbi);   /* до консоли: она сама выберет режим */

    /* КРИТИЧНО для реальных ноутбуков (напр. Samsung RV410 / GMA 4500M).
       GRUB по запросу из Multiboot-заголовка уже ПЕРЕКЛЮЧИЛ карту в
       графику. Если fb_init() этот режим не принял (нестандартный bpp,
       палитровый type, буфер выше 4 ГиБ), то консоль пошла бы писать в
       0xB8000 - а он в графическом режиме не отображается. Получался
       чёрный экран при живом ядре. Возвращаем карту в текст сами. */
    if (!have_fb && !force_text && (mbi->flags & (1u << 12))) {
        /* Диагностика в COM1: видно даже когда на экране ничего нет */
        serial_puts("KvantOS: фреймбуфер загрузчика не принят "
                    "(type/bpp), возврат в текстовый режим VGA\n");
        vbe_force_text();
    }

    vga_init();
    vga_status(" KvantOS — загрузка ядра...", "", VGA_COLOR(VGA_WHITE, VGA_BLUE));

    vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    kputs("\n  KvantOS " KV_VERSION " — запуск ядра\n");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  ──────────────────────────────────────────────────────────\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    if (magic != MULTIBOOT_MAGIC) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf("  Неверная сигнатура Multiboot: 0x%08x\n", magic);
        panic("Загрузчик не совместим с Multiboot 1", NULL);
    }
    step("Multiboot: сигнатура загрузчика проверена");

    gdt_init();
    step("GDT: сегменты ядра и пользователя, TSS");

    idt_init();
    step("IDT: 32 исключения, 16 IRQ, вектор 0x80");

    u32 mem_upper = (mbi->flags & 1) ? mbi->mem_upper : 31744;
    u32 mmap_addr = (mbi->flags & (1 << 6)) ? mbi->mmap_addr : 0;
    u32 mmap_len  = (mbi->flags & (1 << 6)) ? mbi->mmap_length : 0;
    pmm_init(mem_upper, mmap_addr, mmap_len);
    kprintf("       обнаружено %u МиБ ОЗУ, %u страниц по 4 КиБ\n",
            pmm_total_bytes() / 1048576, pmm_total_frames());
    step("PMM: битовая карта физических страниц");

    paging_init();
    if (have_fb && !fb_map())
        panic("Не удалось отобразить фреймбуфер в адресное пространство", NULL);
    step("Paging: identity-mapping 16 МиБ, обработчик #PF");

    heap_init(HEAP_BASE, HEAP_SIZE);
    pmm_reserve_range(HEAP_BASE, HEAP_SIZE);   /* чтобы PMM не выдал эти кадры */
    /* Область, куда разворачиваются приложения .kapp (14-16 МиБ).
       Держим её вне кучи: приложение пишется по фиксированному
       адресу, и кусок кучи там оказаться не должен. */
    pmm_reserve_range(KAPP_LOAD_BASE, KAPP_MAX_SIZE);
    step("Heap: куча ядра 10 МиБ (first-fit + слияние)");

    /* 1000 Гц вместо 100. При шаге 10 мс кадр невозможно отмерить точнее
       33 к/с, а task_sleep(1) спал бы все 10 мс. Миллисекундный тик
       снимает это ограничение; накладные расходы на IRQ ничтожны. */
    timer_init(1000);
    step("PIT: системный таймер 1000 Гц (шаг 1 мс)");

    keyboard_init();
    /* Диагностика для машин без COM-порта (ноутбуки): зажигаем NumLock.
       Если экран чёрный, но NumLock загорелся - ядро живо и дошло сюда. */
    kbd_set_leds(0x02);
    step("PS/2: драйвер клавиатуры (скан-коды набора 1)");

    if (safe_mode) {
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kputs("  [--] Безопасный режим: PCI, VBE и мышь пропущены\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        goto skip_hw;
    }

    pci_init();
    {
        pci_dev_t *g = pci_gpu();
        if (g) kprintf("       видеокарта: %s (%04x:%04x)\n",
                       pci_gpu_model(g->vendor, g->device), g->vendor, g->device);
    }
    kprintf("       найдено устройств PCI: %u\n", pci_count());
    step("PCI: шина просканирована");

    vbe_init();
    kprintf("       интерфейс: %s\n", vbe_backend_name());
    step(vbe_can_modeset()
         ? "VBE: смена разрешения доступна (команда vidmode)"
         : "VBE: режим зафиксирован загрузчиком");

skip_hw:
    if (have_fb && !safe_mode) {
        mouse_init((i32)fb_width(), (i32)fb_height());
        kprintf("       фреймбуфер %ux%u, %u бит на пиксель\n",
                fb_width(), fb_height(), fb_bpp_get());
        step("VBE: линейный фреймбуфер + мышь PS/2 (команда guimenu)");
    } else {
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kputs(force_text
              ? "  [--] Запрошен текстовый режим: VGA 80x25, CRTC доступен\n"
              : "  [--] Графический режим недоступен, только текстовая консоль\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }

    ramfs_init();
    step("ramfs: файловая система в ОЗУ смонтирована");

    /* Приложения, вложенные в загрузочный образ.
       GRUB кладёт их в память и передаёт список в mods_addr.
       Так .kapp попадает в систему даже без диска и без сети:
       достаточно записать ISO. Дальше их можно поставить на диск
       командой install или запустить прямо из ramfs. */
    if ((mbi->flags & (1u << 3)) && mbi->mods_count) {
        struct mb_module { u32 start, end, string, reserved; };
        struct mb_module *mod = (struct mb_module *)mbi->mods_addr;
        u32 added = 0;
        for (u32 i = 0; i < mbi->mods_count && i < 16; i++) {
            u32 size = mod[i].end - mod[i].start;
            if (!size || size > 0x00100000) continue;    /* пустой или неприлично большой */

            /* Имя берём из строки модуля: GRUB кладёт туда то,
               что указано в grub.cfg вторым словом. Оставляем
               только последний путь после слэша. */
            const char *nm = (const char *)mod[i].string;
            const char *base = nm;
            if (nm) for (const char *q = nm; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
            if (!base || !base[0]) base = "module.kapp";

            if (ramfs_create(base, (const char *)mod[i].start, size) == 0) added++;
        }
        if (added) {
            kprintf("       приложений из образа: %u\n", added);
            step("Модули: приложения загружены в ramfs");
        }
    }

    /* Диск: без него система работает, но приложения ставить некуда */
    if (!safe_mode) {
        ata_init();
        if (ata_present()) {
            int d = ata_boot_drive();
            kprintf("       %s, %u МиБ\n", ata_model(d), ata_size_mb(d));
            int m = kvfs_mount();
            if (m == 0) {
                u32 mb, kb, nf;
                kvfs_stats(&mb, &kb, &nf);
                kprintf("       KvFS: файлов %u, занято %u КиБ\n", nf, kb);
                step("Диск: KvFS подключён (приложения сохраняются)");
            } else if (m == -3) {
                step("Диск: найден, но не размечен (команда format)");
            } else {
                kprintf("       код %d: %s\n", m, kvfs_error(m));
                step("Диск: найден, ФС не подключена");
            }
        } else {
            vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            kputs("  [--] Диск ATA не найден: установка приложений недоступна\n");
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        }
    }

    sched_init();
    task_create("idle", idle_task);
    step("Планировщик: round-robin с вытеснением");

    if (mbi->flags & (1 << 9))
        kprintf("       загрузчик: %s\n", (const char *)mbi->boot_loader_name);

    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs("\n  Ядро готово. Запуск оболочки kvsh...\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    sti();

    /* Ядро полностью загружено. Три светодиода + короткий сигнал:
       на ноутбуке без COM-порта это единственное подтверждение
       успешного старта, если изображения на экране нет. */
    kbd_set_leds(0x07);
    sleep_ms(150);
    kbd_set_leds(0x00);
    beep(880, 120);

    sleep_ms(600);
    vga_clear();

    shell_run();

    panic("Оболочка неожиданно завершилась", NULL);
}
