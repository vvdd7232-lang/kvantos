/* ============================================================
 *  KvantOS - kernel entry point
 * ============================================================ */
#include "kernel.h"
#include "kvapp.h"

#define MULTIBOOT_MAGIC 0x2BADB002

#define HEAP_BASE 0x01000000      /* 16 MiB (above the .kapp window) */
#define HEAP_SIZE 0x04000000      /* 64 MiB: room for large screen buffers */

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

    /* The bootloader may pass the word text on the command line -
       then we force the classic VGA 80x25 mode. */
    int force_text = 0, safe_mode = 0;
    if ((mbi->flags & (1u << 2)) && mbi->cmdline) {
        const char *p = (const char *)mbi->cmdline;
        for (; *p; p++) {
            if (p[0] == 't' && p[1] == 'e' && p[2] == 'x' && p[3] == 't') force_text = 1;
            if (p[0] == 's' && p[1] == 'a' && p[2] == 'f' && p[3] == 'e') safe_mode = 1;
            /* lang=ru switches the whole system to Russian */
            if (p[0] == 'l' && p[1] == 'a' && p[2] == 'n' && p[3] == 'g' &&
                p[4] == '=' && p[5] == 'r' && p[6] == 'u') kv_lang_set(KV_LANG_RU);
        }
    }
    if (safe_mode) force_text = 1;   /* safe mode is always textual */

    /* GRUB has already switched to graphics as requested by the
       Multiboot header, so for text mode we put the adapter back. */
    if (force_text) vbe_force_text();

    int have_fb = force_text ? 0 : fb_init(mbi);   /* before the console: it picks its own mode */

    /* CRITICAL on real laptops (e.g. Samsung RV410 / GMA 4500M).
       Following the Multiboot header request GRUB has ALREADY switched
       the adapter into graphics. If fb_init() rejected that mode
       (unusual bpp, palette type, a buffer above 4 GiB) the console
       would write to 0xB8000 - which is not displayed in graphics mode.
       The result was a black screen with a perfectly alive kernel. So
       we switch the adapter back to text ourselves. */
    if (!have_fb && !force_text && (mbi->flags & (1u << 12))) {
        /* Diagnostics on COM1: visible even when the screen shows nothing */
        serial_puts(T("KvantOS: bootloader framebuffer rejected " "(type/bpp), falling back to VGA text mode\n", "KvantOS: фреймбуфер загрузчика не принят " "(type/bpp), возврат в текстовый режим VGA\n"));
        vbe_force_text();
    }

    vga_init();
    vga_status(T(" KvantOS — loading the kernel...", " KvantOS — загрузка ядра..."), "", VGA_COLOR(VGA_WHITE, VGA_BLUE));

    vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    kputs("\n  KvantOS " KV_VERSION);
    kputs(T(" — kernel start\n", " — запуск ядра\n"));
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  ──────────────────────────────────────────────────────────\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    if (magic != MULTIBOOT_MAGIC) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf(T("  Bad Multiboot signature: 0x%08x\n", "  Неверная сигнатура Multiboot: 0x%08x\n"), magic);
        panic(T("Bootloader is not Multiboot 1 compatible", "Загрузчик не совместим с Multiboot 1"), NULL);
    }
    step(T("Multiboot: bootloader signature verified", "Multiboot: сигнатура загрузчика проверена"));

    gdt_init();
    step(T("GDT: kernel and user segments, TSS", "GDT: сегменты ядра и пользователя, TSS"));

    idt_init();
    step(T("IDT: 32 exceptions, 16 IRQs, vector 0x80", "IDT: 32 исключения, 16 IRQ, вектор 0x80"));

    u32 mem_upper = (mbi->flags & 1) ? mbi->mem_upper : 31744;
    u32 mmap_addr = (mbi->flags & (1 << 6)) ? mbi->mmap_addr : 0;
    u32 mmap_len  = (mbi->flags & (1 << 6)) ? mbi->mmap_length : 0;
    pmm_init(mem_upper, mmap_addr, mmap_len);
    kprintf(T("       detected %u MiB RAM, %u pages of 4 KiB\n", "       обнаружено %u МиБ ОЗУ, %u страниц по 4 КиБ\n"),
            pmm_total_bytes() / 1048576, pmm_total_frames());
    step(T("PMM: physical page bitmap", "PMM: битовая карта физических страниц"));

    paging_init();
    if (have_fb && !fb_map())
        panic(T("Failed to map the framebuffer into the address space", "Не удалось отобразить фреймбуфер в адресное пространство"), NULL);
    step(T("Paging: identity-mapped 128 MiB, #PF handler", "Paging: identity-mapping 128 МиБ, обработчик #PF"));

    /* The heap is sized from the RAM actually present. The desktop wants
       two screen-sized buffers (the back buffer plus the cached
       background) - at 1920x1080x32 that is 16 MiB on its own - but a
       machine with 32 MiB of RAM must still boot, so the heap never
       takes more than about half of physical memory. */
    u32 heap_size = HEAP_SIZE;
    u32 ram = pmm_total_bytes();
    if (ram < HEAP_BASE + HEAP_SIZE + (8u << 20)) {
        u32 avail = (ram > HEAP_BASE) ? (ram - HEAP_BASE) : 0;
        heap_size = avail / 2;
        heap_size &= ~0xFFFFFu;                     /* round down to 1 MiB */
        if (heap_size > HEAP_SIZE) heap_size = HEAP_SIZE;
        if (heap_size < (2u << 20)) heap_size = (2u << 20);   /* bare minimum */
    }

    heap_init(HEAP_BASE, heap_size);
    pmm_reserve_range(HEAP_BASE, heap_size);   /* so that the PMM never hands out these frames */
    /* The region where .kapp applications are unpacked (14-16 MiB).
       It is kept outside the heap: an application is written to a fixed
       address and no heap chunk may end up there. */
    pmm_reserve_range(KAPP_LOAD_BASE, KAPP_MAX_SIZE);
    kprintf(T("       heap %u MiB at %u MiB\n", "       куча %u МиБ по адресу %u МиБ\n"), heap_size / 1048576, HEAP_BASE / 1048576);
    step(T("Heap: kernel heap (first-fit + coalescing)", "Heap: куча ядра (first-fit + слияние)"));

    /* 1000 Hz instead of 100. With a 10 ms step a frame cannot be timed
       more precisely than 33 fps, and task_sleep(1) would sleep the full
       10 ms. A millisecond tick removes that limit; the IRQ overhead is
       negligible. */
    timer_init(1000);
    step(T("PIT: system timer at 1000 Hz (1 ms tick)", "PIT: системный таймер 1000 Гц (шаг 1 мс)"));

    keyboard_init();
    /* Diagnostics for machines without a COM port (laptops): light up
       NumLock. A black screen with NumLock lit means the kernel is alive
       and got this far. */
    kbd_set_leds(0x02);
    step(T("PS/2: keyboard driver (scan code set 1)", "PS/2: драйвер клавиатуры (скан-коды набора 1)"));

    if (safe_mode) {
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kputs(T("  [--] Safe mode: PCI, VBE and mouse skipped\n", "  [--] Безопасный режим: PCI, VBE и мышь пропущены\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        goto skip_hw;
    }

    pci_init();
    {
        pci_dev_t *g = pci_gpu();
        if (g) kprintf(T("       adapter: %s (%04x:%04x)\n", "       видеокарта: %s (%04x:%04x)\n"),
                       pci_gpu_model(g->vendor, g->device), g->vendor, g->device);
    }
    kprintf(T("       PCI devices found: %u\n", "       найдено устройств PCI: %u\n"), pci_count());
    step(T("PCI: bus scanned", "PCI: шина просканирована"));

    vbe_init();
    kprintf(T("       interface: %s\n", "       интерфейс: %s\n"), vbe_backend_name());
    step(vbe_can_modeset()
         ? T("VBE: resolution switching available (vidmode command)", "VBE: смена разрешения доступна (команда vidmode)")
         : T("VBE: mode fixed by the bootloader", "VBE: режим зафиксирован загрузчиком"));

skip_hw:
    if (have_fb && !safe_mode) {
        mouse_init((i32)fb_width(), (i32)fb_height());
        kprintf(T("       framebuffer %ux%u, %u bits per pixel\n", "       фреймбуфер %ux%u, %u бит на пиксель\n"),
                fb_width(), fb_height(), fb_bpp_get());
        step(T("VBE: linear framebuffer + PS/2 mouse (guimenu command)", "VBE: линейный фреймбуфер + мышь PS/2 (команда guimenu)"));
    } else {
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kputs(force_text
              ? T("  [--] Text mode requested: VGA 80x25, CRTC available\n", "  [--] Запрошен текстовый режим: VGA 80x25, CRTC доступен\n")
              : T("  [--] No graphics mode available, text console only\n", "  [--] Графический режим недоступен, только текстовая консоль\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }

    ramfs_init();
    step(T("ramfs: in-memory filesystem mounted", "ramfs: файловая система в ОЗУ смонтирована"));

    /* Applications embedded in the boot image.
       GRUB places them in memory and passes the list in mods_addr.
       That is how a .kapp reaches the system with no disk and no
       network: burning the ISO is enough. From there they can be put on
       a disk with the install command or run straight from ramfs. */
    if ((mbi->flags & (1u << 3)) && mbi->mods_count) {
        struct mb_module { u32 start, end, string, reserved; };
        struct mb_module *mod = (struct mb_module *)mbi->mods_addr;
        u32 added = 0;
        for (u32 i = 0; i < mbi->mods_count && i < 16; i++) {
            u32 size = mod[i].end - mod[i].start;
            if (!size || size > 0x00100000) continue;    /* empty or indecently large */

            /* The name comes from the module string: GRUB puts there
               whatever grub.cfg lists as the second word. Only the last
               path component after the slash is kept. */
            const char *nm = (const char *)mod[i].string;
            const char *base = nm;
            if (nm) for (const char *q = nm; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
            if (!base || !base[0]) base = "module.kapp";

            if (ramfs_create(base, (const char *)mod[i].start, size) == 0) added++;
        }
        if (added) {
            kprintf(T("       applications from the image: %u\n", "       приложений из образа: %u\n"), added);
            step(T("Modules: applications loaded into ramfs", "Модули: приложения загружены в ramfs"));
        }
    }

    /* The disk: the system works without one, but there is nowhere to install applications */
    if (!safe_mode) {
        ata_init();
        if (ata_present()) {
            int d = ata_boot_drive();
            kprintf(T("       %s, %u MiB\n", "       %s, %u МиБ\n"), ata_model(d), ata_size_mb(d));
            int m = kvfs_mount();
            if (m == 0) {
                u32 mb, kb, nf;
                kvfs_stats(&mb, &kb, &nf);
                kprintf(T("       KvFS: %u files, %u KiB used\n", "       KvFS: файлов %u, занято %u КиБ\n"), nf, kb);
                step(T("Disk: KvFS mounted (applications persist)", "Диск: KvFS подключён (приложения сохраняются)"));
            } else if (m == -3) {
                step(T("Disk: found but not formatted (format command)", "Диск: найден, но не размечен (команда format)"));
            } else {
                kprintf(T("       code %d: %s\n", "       код %d: %s\n"), m, kvfs_error(m));
                step(T("Disk: found, filesystem not mounted", "Диск: найден, ФС не подключена"));
            }
        } else {
            vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            kputs(T("  [--] No ATA disk: installing applications is unavailable\n", "  [--] Диск ATA не найден: установка приложений недоступна\n"));
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        }
    }

    sched_init();
    task_create("idle", idle_task);
    step(T("Scheduler: pre-emptive round-robin", "Планировщик: round-robin с вытеснением"));

    if (mbi->flags & (1 << 9))
        kprintf(T("       bootloader: %s\n", "       загрузчик: %s\n"), (const char *)mbi->boot_loader_name);

    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kputs(T("\n  Kernel ready. Starting the kvsh shell...\n", "\n  Ядро готово. Запуск оболочки kvsh...\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    sti();

    /* The kernel is fully up. Three LEDs plus a short beep: on a laptop
       without a COM port that is the only confirmation of a successful
       start when there is no picture on the screen. */
    kbd_set_leds(0x07);
    sleep_ms(150);
    kbd_set_leds(0x00);
    beep(880, 120);

    sleep_ms(600);
    vga_clear();

    shell_run();

    panic(T("The shell exited unexpectedly", "Оболочка неожиданно завершилась"), NULL);
}
