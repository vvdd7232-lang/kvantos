/* ============================================================
 *  KvantOS - команды управления видеокартой и видеорежимом
 * ============================================================ */
#include "kernel.h"

/* ---------------- lspci ---------------- */
void cmd_lspci(int verbose) {
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("  Устройства на шине PCI\n");
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  ────────────────────────────────────────────────────────────\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    u32 n = pci_count();
    for (u32 i = 0; i < n; i++) {
        pci_dev_t *d = pci_get(i);
        vga_set_color(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
        kprintf("  %02x:%02x.%u", d->bus, d->slot, d->func);
        vga_set_color(VGA_COLOR(VGA_WHITE, VGA_BLACK));
        kprintf("  %04x:%04x", d->vendor, d->device);
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kprintf("  %s\n", pci_class_name(d->class_code, d->subclass));
        kprintf("            %s", pci_vendor_name(d->vendor));
        if (d->class_code == 0x03)
            kprintf(" — %s", pci_gpu_model(d->vendor, d->device));
        kputc('\n');

        if (verbose) {
            vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
            kprintf("            класс %02x:%02x, rev %02x, IRQ %u\n",
                    d->class_code, d->subclass, d->revision, d->irq);
            for (int b = 0; b < 6; b++) {
                if (!d->bar[b]) continue;
                if (d->bar_is_io[b])
                    kprintf("            BAR%d: порты 0x%04x\n", b, d->bar[b] & 0xFFFC);
                else {
                    u32 sz = pci_bar_size(d, b);
                    kprintf("            BAR%d: память %p", b, (void *)(d->bar[b] & 0xFFFFFFF0u));
                    if (sz >= 1048576) kprintf(" (%u МиБ)", sz / 1048576);
                    else if (sz) kprintf(" (%u КиБ)", sz / 1024);
                    kputc('\n');
                }
            }
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        }
    }
    kprintf("\n  Всего устройств: %u\n\n", n);
}

/* ---------------- сведения о видеокарте ---------------- */
void cmd_gpuinfo(void) {
    pci_dev_t *g = pci_gpu();
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("  Видеокарта\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    if (!g) {
        kputs("    PCI-видеоадаптер не обнаружен\n\n");
        return;
    }

    kprintf("    модель      : %s\n", pci_gpu_model(g->vendor, g->device));
    kprintf("    производит. : %s (0x%04x)\n", pci_vendor_name(g->vendor), g->vendor);
    kprintf("    device id   : 0x%04x, ревизия %02x\n", g->device, g->revision);
    kprintf("    адрес PCI   : %02x:%02x.%u\n", g->bus, g->slot, g->func);

    int idx = -1;
    u32 bar = pci_bar_mem(g, &idx);
    if (idx >= 0) {
        u32 sz = pci_bar_size(g, idx);
        kprintf("    видеопамять : %p", (void *)bar);
        if (sz >= 1048576) kprintf(", %u МиБ\n", sz / 1048576);
        else if (sz) kprintf(", %u КиБ\n", sz / 1024);
        else kputc('\n');
    }

    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("\n  Драйвер смены режима\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("    интерфейс   : %s\n", vbe_backend_name());
    if (vbe_backend() == GPU_BGA)
        kprintf("    версия BGA  : 0x%04x\n", vbe_bga_version());
    kprintf("    смена режима: %s\n",
            vbe_can_modeset() ? "поддерживается" : "недоступна");

    vmode_t m;
    vbe_current(&m);
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("\n  Текущий режим\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf("    %u x %u, %u бит, шаг %u байт\n", m.width, m.height, m.bpp, m.pitch);
    kprintf("    буфер кадра : %p (%u КиБ)\n", (void *)m.phys, fb_bytes() / 1024);
    kprintf("    консоль     : %u x %u символов\n", vga_cols(), vga_rows());
    kputc('\n');
}

/* ---------------- список режимов ---------------- */
void cmd_vidmode_list(void) {
    vmode_t cur, m;
    vbe_current(&cur);
    u32 vram = vbe_vram_bytes();

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs("   №   РАЗРЕШЕНИЕ    ГЛУБИНА   ПАМЯТЬ    СТАТУС\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    u32 n = vbe_mode_count();
    for (u32 i = 0; i < n; i++) {
        vbe_mode_get(i, &m);
        u32 need = m.width * m.height * (m.bpp >> 3);
        int fits = (vram == 0) || (need <= vram);
        int active = (m.width == cur.width && m.height == cur.height && m.bpp == cur.bpp);

        if (active) vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        else if (!fits) vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));

        kprintf("   %u", i + 1);
        if (i + 1 < 10) kputc(' ');
        kprintf("   %u x %u", m.width, m.height);
        u32 pad = 13 - (m.width >= 1000 ? 4 : 3) - (m.height >= 1000 ? 4 : 3) - 3;
        for (u32 s = 0; s < pad; s++) kputc(' ');
        kprintf("%u бит", m.bpp);
        kputs(m.bpp == 32 ? "    " : "    ");
        kprintf("%u КиБ", need / 1024);
        if (need / 1024 < 1000) kputc(' ');
        kputs("   ");
        if (active) kputs("← текущий");
        else if (!fits) kputs("мало видеопамяти");
        else kputs("доступен");
        kputc('\n');
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }

    if (vram)
        kprintf("\n  Видеопамять: %u МиБ\n", vram / 1048576);
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs("  Смена режима: vidmode <номер>  либо  vidmode 1280 720 32\n\n");
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

/* Применить режим и перерисовать консоль */
static void apply_mode(u32 w, u32 h, u32 bpp) {
    kprintf("\n  Переключение в %u x %u, %u бит...\n", w, h, bpp);
    sleep_ms(250);

    int r = vbe_set_mode(w, h, bpp);
    if (r != VBE_OK) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf("  Ошибка: %s\n\n", vbe_error_text(r));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }

    fb_console_resync();
    logo_print();
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kprintf("\n  Режим изменён: %u x %u, %u бит  (консоль %u x %u символов)\n",
            fb_width(), fb_height(), fb_bpp_get(), vga_cols(), vga_rows());
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs("  Проверка палитры: ");
    for (int i = 0; i < 8; i++) {
        static const u8 cl[8] = { VGA_LRED, VGA_YELLOW, VGA_LGREEN, VGA_LCYAN,
                                  VGA_LBLUE, VGA_LMAGENTA, VGA_WHITE, VGA_LGREY };
        vga_set_color(VGA_COLOR(cl[i], VGA_BLACK));
        kputs("██");
    }
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs("\n\n");
}

void cmd_vidmode(int argc, char **argv) {
    if (!fb_active()) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kputs("\n  Система загружена в текстовом режиме VGA.\n");
        kputs("  Выберите графический пункт в меню GRUB.\n\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }

    if (argc < 2) { cmd_vidmode_list(); return; }

    if (!vbe_can_modeset()) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf("\n  %s\n", vbe_error_text(VBE_ERR_UNSUPPORTED));
        vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
        kputs("  Обнаружен интерфейс: ");
        kprintf("%s\n\n", vbe_backend_name());
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }

    /* vidmode <номер из списка> */
    if (argc == 2) {
        int idx = atoi(argv[1]);
        vmode_t m;
        if (idx < 1 || !vbe_mode_get((u32)(idx - 1), &m)) {
            kprintf("\n  Нет режима с номером %d. Список: vidmode\n\n", idx);
            return;
        }
        apply_mode(m.width, m.height, m.bpp);
        return;
    }

    /* vidmode <ширина> <высота> [глубина] */
    u32 w = (u32)atoi(argv[1]);
    u32 h = (u32)atoi(argv[2]);
    u32 bpp = argc > 3 ? (u32)atoi(argv[3]) : 32;
    apply_mode(w, h, bpp);
}

/* ---------------- частота обновления ---------------- */
void cmd_refresh(int argc, char **argv) {
    if (!fb_active() && vbe_backend() == GPU_NONE) {
        kputs("\n  Видеоподсистема недоступна\n\n");
        return;
    }

    if (argc < 2) {
        kputc('\n');
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kputs("  Частота обновления экрана\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kprintf("    настроено   : %u Гц\n", vbe_get_refresh());

        u32 crtc = vbe_measure_hz();
        if (crtc) kprintf("    расчёт CRTC : %u Гц\n", crtc);
        else      kputs("    расчёт CRTC : неприменим в режиме LFB\n");

        kputs("    замер VSync : ");
        u32 real = vbe_count_vsync();
        if (real) kprintf("%u кадр/с (измерено)\n", real);
        else kputs("развёртка не эмулируется видеокартой\n");

        vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
        kputs("\n    Изменить: refresh <50..120>\n");
        if (fb_active() && (vbe_backend() == GPU_BGA || vbe_backend() == GPU_VMWARE))
            kputs("    Внимание: в режиме LFB развёртку задаёт хост-система\n");
        else
            kputs("    Тайминги CRTC программируются напрямую\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputc('\n');
        return;
    }

    u32 hz = (u32)atoi(argv[1]);
    int r = vbe_set_refresh(hz);

    if (r == VBE_OK) {
        vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        kprintf("\n  Частота обновления установлена: %u Гц\n", hz);
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        u32 chk = vbe_measure_hz();
        if (chk) kprintf("  Проверка по CRTC: %u Гц\n\n", chk);
        else kputc('\n');
    } else if (r == VBE_WARN_VIRTUAL) {
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kprintf("\n  Значение %u Гц сохранено, но реальную развёртку\n", hz);
        kputs("  в виртуальной машине задаёт хост-система.\n");
        vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
        kputs("  На физическом железе с обычным CRTC частота меняется.\n\n");
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    } else {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf("\n  Ошибка: %s\n\n", vbe_error_text(r));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }
}
