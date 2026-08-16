/* ============================================================
 *  KvantOS - commands controlling the adapter and the video mode
 * ============================================================ */
#include "kernel.h"

/* ---------------- lspci ---------------- */
void cmd_lspci(int verbose) {
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  PCI bus devices\n", "  Устройства на шине PCI\n"));
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
            kprintf(T("            class %02x:%02x, rev %02x, IRQ %u\n", "            класс %02x:%02x, rev %02x, IRQ %u\n"),
                    d->class_code, d->subclass, d->revision, d->irq);
            for (int b = 0; b < 6; b++) {
                if (!d->bar[b]) continue;
                if (d->bar_is_io[b])
                    kprintf(T("            BAR%d: ports 0x%04x\n", "            BAR%d: порты 0x%04x\n"), b, d->bar[b] & 0xFFFC);
                else {
                    u32 sz = pci_bar_size(d, b);
                    kprintf(T("            BAR%d: memory %p", "            BAR%d: память %p"), b, (void *)(d->bar[b] & 0xFFFFFFF0u));
                    if (sz >= 1048576) kprintf(T(" (%u MiB)", " (%u МиБ)"), sz / 1048576);
                    else if (sz) kprintf(T(" (%u KiB)", " (%u КиБ)"), sz / 1024);
                    kputc('\n');
                }
            }
            vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        }
    }
    kprintf(T("\n  Devices total: %u\n\n", "\n  Всего устройств: %u\n\n"), n);
}

/* ---------------- graphics adapter information ---------------- */
void cmd_gpuinfo(void) {
    pci_dev_t *g = pci_gpu();
    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("  Graphics adapter\n", "  Видеокарта\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));

    if (!g) {
        kputs(T("    no PCI display adapter found\n\n", "    PCI-видеоадаптер не обнаружен\n\n"));
        return;
    }

    kprintf(T("    model       : %s\n", "    модель      : %s\n"), pci_gpu_model(g->vendor, g->device));
    kprintf(T("    vendor      : %s (0x%04x)\n", "    производит. : %s (0x%04x)\n"), pci_vendor_name(g->vendor), g->vendor);
    kprintf(T("    device id   : 0x%04x, revision %02x\n", "    device id   : 0x%04x, ревизия %02x\n"), g->device, g->revision);
    kprintf(T("    PCI address : %02x:%02x.%u\n", "    адрес PCI   : %02x:%02x.%u\n"), g->bus, g->slot, g->func);

    int idx = -1;
    u32 bar = pci_bar_mem(g, &idx);
    if (idx >= 0) {
        u32 sz = pci_bar_size(g, idx);
        kprintf(T("    video memory: %p", "    видеопамять : %p"), (void *)bar);
        if (sz >= 1048576) kprintf(T(", %u MiB\n", ", %u МиБ\n"), sz / 1048576);
        else if (sz) kprintf(T(", %u KiB\n", ", %u КиБ\n"), sz / 1024);
        else kputc('\n');
    }

    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("\n  Mode-setting driver\n", "\n  Драйвер смены режима\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf(T("    interface   : %s\n", "    интерфейс   : %s\n"), vbe_backend_name());
    if (vbe_backend() == GPU_BGA)
        kprintf(T("    BGA version : 0x%04x\n", "    версия BGA  : 0x%04x\n"), vbe_bga_version());
    kprintf(T("    mode setting: %s\n", "    смена режима: %s\n"),
            vbe_can_modeset() ? T("supported", "поддерживается") : T("unavailable", "недоступна"));

    vmode_t m;
    vbe_current(&m);
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("\n  Current mode\n", "\n  Текущий режим\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kprintf(T("    %u x %u, %u bpp, pitch %u bytes\n", "    %u x %u, %u бит, шаг %u байт\n"), m.width, m.height, m.bpp, m.pitch);
    kprintf(T("    framebuffer : %p (%u KiB)\n", "    буфер кадра : %p (%u КиБ)\n"), (void *)m.phys, fb_bytes() / 1024);
    kprintf(T("    console     : %u x %u characters\n", "    консоль     : %u x %u символов\n"), vga_cols(), vga_rows());
    kputc('\n');
}

/* ---------------- mode list ---------------- */
void cmd_vidmode_list(void) {
    vmode_t cur, m;
    vbe_current(&cur);
    u32 vram = vbe_vram_bytes();

    kputc('\n');
    vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    kputs(T("   #   RESOLUTION    DEPTH     MEMORY    STATUS\n", "   №   РАЗРЕШЕНИЕ    ГЛУБИНА   ПАМЯТЬ    СТАТУС\n"));
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
        kprintf(T("%u bpp", "%u бит"), m.bpp);
        kputs(m.bpp == 32 ? "    " : "    ");
        kprintf(T("%u KiB", "%u КиБ"), need / 1024);
        if (need / 1024 < 1000) kputc(' ');
        kputs("   ");
        if (active) kputs(T("← current", "← текущий"));
        else if (!fits) kputs(T("not enough VRAM", "мало видеопамяти"));
        else kputs(T("available", "доступен"));
        kputc('\n');
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }

    if (vram)
        kprintf(T("\n  Video memory: %u MiB\n", "\n  Видеопамять: %u МиБ\n"), vram / 1048576);
    vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
    kputs(T("  Change mode: vidmode <number>  or  vidmode 1280 720 32\n\n", "  Смена режима: vidmode <номер>  либо  vidmode 1280 720 32\n\n"));
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
}

/* Apply the mode and redraw the console */
static void apply_mode(u32 w, u32 h, u32 bpp) {
    kprintf(T("\n  Switching to %u x %u, %u bpp...\n", "\n  Переключение в %u x %u, %u бит...\n"), w, h, bpp);
    sleep_ms(250);

    int r = vbe_set_mode(w, h, bpp);
    if (r != VBE_OK) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf(T("  Error: %s\n\n", "  Ошибка: %s\n\n"), vbe_error_text(r));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }

    fb_console_resync();
    logo_print();
    vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    kprintf(T("\n  Mode changed: %u x %u, %u bpp  (console %u x %u characters)\n", "\n  Режим изменён: %u x %u, %u бит  (консоль %u x %u символов)\n"),
            fb_width(), fb_height(), fb_bpp_get(), vga_cols(), vga_rows());
    vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    kputs(T("  Palette check: ", "  Проверка палитры: "));
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
        kputs(T("\n  The system booted in VGA text mode.\n", "\n  Система загружена в текстовом режиме VGA.\n"));
        kputs(T("  Pick a graphical entry in the GRUB menu.\n\n", "  Выберите графический пункт в меню GRUB.\n\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }

    if (argc < 2) { cmd_vidmode_list(); return; }

    if (!vbe_can_modeset()) {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf("\n  %s\n", vbe_error_text(VBE_ERR_UNSUPPORTED));
        vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
        kputs(T("  Detected interface: ", "  Обнаружен интерфейс: "));
        kprintf("%s\n\n", vbe_backend_name());
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        return;
    }

    /* vidmode <number from the list> */
    if (argc == 2) {
        int idx = atoi(argv[1]);
        vmode_t m;
        if (idx < 1 || !vbe_mode_get((u32)(idx - 1), &m)) {
            kprintf(T("\n  No mode number %d. List them with: vidmode\n\n", "\n  Нет режима с номером %d. Список: vidmode\n\n"), idx);
            return;
        }
        apply_mode(m.width, m.height, m.bpp);
        return;
    }

    /* vidmode <width> <height> [depth] */
    u32 w = (u32)atoi(argv[1]);
    u32 h = (u32)atoi(argv[2]);
    u32 bpp = argc > 3 ? (u32)atoi(argv[3]) : 32;
    apply_mode(w, h, bpp);
}

/* ---------------- refresh rate ---------------- */
void cmd_refresh(int argc, char **argv) {
    if (!fb_active() && vbe_backend() == GPU_NONE) {
        kputs(T("\n  Video subsystem unavailable\n\n", "\n  Видеоподсистема недоступна\n\n"));
        return;
    }

    if (argc < 2) {
        kputc('\n');
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kputs(T("  Screen refresh rate\n", "  Частота обновления экрана\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kprintf(T("    configured  : %u Hz\n", "    настроено   : %u Гц\n"), vbe_get_refresh());

        u32 crtc = vbe_measure_hz();
        if (crtc) kprintf(T("    CRTC estimate: %u Hz\n", "    расчёт CRTC : %u Гц\n"), crtc);
        else      kputs(T("    CRTC estimate: not applicable in LFB mode\n", "    расчёт CRTC : неприменим в режиме LFB\n"));

        kputs(T("    VSync measured: ", "    замер VSync : "));
        u32 real = vbe_count_vsync();
        if (real) kprintf(T("%u frames/s (measured)\n", "%u кадр/с (измерено)\n"), real);
        else kputs(T("vertical retrace is not emulated by the adapter\n", "развёртка не эмулируется видеокартой\n"));

        vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
        kputs(T("\n    Change with: refresh <50..120>\n", "\n    Изменить: refresh <50..120>\n"));
        if (fb_active() && (vbe_backend() == GPU_BGA || vbe_backend() == GPU_VMWARE))
            kputs(T("    Note: in LFB mode the host system drives the refresh\n", "    Внимание: в режиме LFB развёртку задаёт хост-система\n"));
        else if (fb_active())
            kputs(T("    Note: in LFB mode the rate is fixed by the video mode\n", "    Внимание: в режиме LFB частота задана видеорежимом\n"));
        else
            kputs(T("    CRTC timings are programmed directly\n", "    Тайминги CRTC программируются напрямую\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        kputc('\n');
        return;
    }

    u32 hz = (u32)atoi(argv[1]);
    int r = vbe_set_refresh(hz);

    if (r == VBE_OK) {
        vga_set_color(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
        kprintf(T("\n  Refresh rate set: %u Hz\n", "\n  Частота обновления установлена: %u Гц\n"), hz);
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
        u32 chk = vbe_measure_hz();
        if (chk) kprintf(T("  CRTC readback: %u Hz\n\n", "  Проверка по CRTC: %u Гц\n\n"), chk);
        else kputc('\n');
    } else if (r == VBE_WARN_VIRTUAL) {
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kprintf(T("\n  The value %u Hz is stored, but the real refresh rate\n", "\n  Значение %u Гц сохранено, но реальную развёртку\n"), hz);
        kputs(T("  inside a virtual machine is set by the host.\n", "  в виртуальной машине задаёт хост-система.\n"));
        vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
        kputs(T("  Outside a linear framebuffer the CRTC is programmed directly.\n\n", "  Вне режима линейного фреймбуфера CRTC программируется напрямую.\n\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    } else if (r == VBE_ERR_UNSUPPORTED && fb_active()) {
        /* Deliberate refusal, not a failure: under a linear framebuffer
           the legacy CRTC does not drive the scan-out, and programming it
           would throw the monitor out of sync. */
        vga_set_color(VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        kputs(T("\n  In linear framebuffer mode the refresh rate is fixed\n",
                "\n  В режиме линейного фреймбуфера частота задана\n"));
        kputs(T("  by the video mode and cannot be reprogrammed safely.\n",
                "  видеорежимом и не может быть изменена безопасно.\n"));
        vga_set_color(VGA_COLOR(VGA_DGREY, VGA_BLACK));
        kputs(T("  Pick another resolution with: vidmode\n\n",
                "  Выберите другое разрешение командой: vidmode\n\n"));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    } else {
        vga_set_color(VGA_COLOR(VGA_LRED, VGA_BLACK));
        kprintf(T("\n  Error: %s\n\n", "\n  Ошибка: %s\n\n"), vbe_error_text(r));
        vga_set_color(VGA_COLOR(VGA_LGREY, VGA_BLACK));
    }
}
