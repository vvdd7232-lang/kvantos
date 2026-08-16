/* ============================================================
 *  KvantOS - video mode control without the BIOS.
 *
 *  int 0x10 cannot be called from protected mode, so the resolution
 *  is changed through the adapter registers directly:
 *
 *   1) Bochs/QEMU/VirtualBox VBE Extensions (BGA) - ports 0x1CE/0x1CF.
 *      They allow arbitrary width, height and colour depth.
 *   2) VMware SVGA II - a register interface through BAR0.
 *   3) The frame rate is set by programming the VGA CRTC timings
 *      (only for modes where the adapter hands over timing control).
 * ============================================================ */
#include "kernel.h"

/* ---- Bochs Graphics Adaptor ---- */
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

#define VBE_DISPI_INDEX_ID          0x0
#define VBE_DISPI_INDEX_XRES        0x1
#define VBE_DISPI_INDEX_YRES        0x2
#define VBE_DISPI_INDEX_BPP         0x3
#define VBE_DISPI_INDEX_ENABLE      0x4
#define VBE_DISPI_INDEX_BANK        0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET    0x8
#define VBE_DISPI_INDEX_Y_OFFSET    0x9

#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_NOCLEARMEM    0x80

#define VBE_DISPI_ID0           0xB0C0
#define VBE_DISPI_ID5           0xB0C5

/* ---- VGA registers ---- */
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5
#define VGA_MISC_W 0x3C2
#define VGA_MISC_R 0x3CC

/* ---- VMware SVGA II ---- */
#define SVGA_INDEX_PORT         0x0
#define SVGA_VALUE_PORT         0x1
#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_BPP            7
#define SVGA_REG_FB_START       13
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_ID_2               0x90000002

static int   backend = GPU_NONE;
static u16   bga_version = 0;
static u16   svga_io = 0;         /* the VMware base I/O port */
static u32   svga_fb = 0;

/* current mode */
static vmode_t cur;

static void vram_probe(void);      /* determined below */

/* ---------- BGA ---------- */
static void bga_write(u16 index, u16 value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static u16 bga_read(u16 index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static int bga_detect(void) {
    u16 id = bga_read(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID0 || id > VBE_DISPI_ID5 + 5) return 0;
    bga_version = id;
    return 1;
}

/* ---------- VMware SVGA ---------- */
static void svga_write(u32 reg, u32 val) {
    outl(svga_io + SVGA_INDEX_PORT, reg);
    outl(svga_io + SVGA_VALUE_PORT, val);
}
static u32 svga_read(u32 reg) {
    outl(svga_io + SVGA_INDEX_PORT, reg);
    return inl(svga_io + SVGA_VALUE_PORT);
}

static int svga_detect(pci_dev_t *gpu) {
    if (!gpu || gpu->vendor != 0x15AD) return 0;
    for (int i = 0; i < 6; i++) {
        if (gpu->bar_is_io[i] && (gpu->bar[i] & 0xFFFC)) {
            svga_io = (u16)(gpu->bar[i] & 0xFFFC);
            break;
        }
    }
    if (!svga_io) return 0;
    svga_write(SVGA_REG_ID, SVGA_ID_2);
    if (svga_read(SVGA_REG_ID) != SVGA_ID_2) return 0;
    svga_fb = svga_read(SVGA_REG_FB_START);
    return 1;
}

/* ---------- programming VGA text mode 80x25 ----------
   Switching BGA off is not enough: QEMU keeps the CRTC registers of
   the graphics mode and the scan-out stays at 1024x768. So the full
   register set of the standard mode 0x03 (720x400, a 9x16 cell) is
   loaded by hand. */

#define VGA_AC_INDEX  0x3C0
#define VGA_AC_WRITE  0x3C0
#define VGA_SEQ_INDEX 0x3C4
#define VGA_SEQ_DATA  0x3C5
#define VGA_GC_INDEX  0x3CE
#define VGA_GC_DATA   0x3CF
#define VGA_INSTAT    0x3DA

static const u8 mode3_seq[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };

static const u8 mode3_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};

static const u8 mode3_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};

static const u8 mode3_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

int vbe_force_text(void) {
    int had_bga = bga_detect();
    if (had_bga) {
        bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        for (volatile int i = 0; i < 200000; i++) {}
    }

    u32 fl = irq_save();

    /* the clock rate and sync polarity of mode 0x03 */
    outb(VGA_MISC_W, 0x67);

    /* Sequencer: reset, load, release the reset */
    outb(VGA_SEQ_INDEX, 0x00); outb(VGA_SEQ_DATA, 0x01);
    for (u8 i = 1; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        outb(VGA_SEQ_DATA, mode3_seq[i]);
    }
    outb(VGA_SEQ_INDEX, 0x00); outb(VGA_SEQ_DATA, 0x03);

    /* CRTC: unprotect registers 0-7, then load all 25 */
    outb(CRTC_INDEX, 0x11);
    outb(CRTC_DATA, (u8)(inb(CRTC_DATA) & 0x7F));
    for (u8 i = 0; i < 25; i++) {
        outb(CRTC_INDEX, i);
        outb(CRTC_DATA, mode3_crtc[i]);
    }

    /* Graphics Controller */
    for (u8 i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        outb(VGA_GC_DATA, mode3_gc[i]);
    }

    /* Attribute Controller: reset the flip-flop by reading 0x3DA */
    (void)inb(VGA_INSTAT);
    for (u8 i = 0; i < 21; i++) {
        (void)inb(VGA_INSTAT);
        outb(VGA_AC_INDEX, i);
        outb(VGA_AC_WRITE, mode3_ac[i]);
    }
    (void)inb(VGA_INSTAT);
    outb(VGA_AC_INDEX, 0x20);      /* enable output to the screen */

    irq_restore(fl);
    return 1;
}

/* ---------- initialisation ---------- */
void vbe_init(void) {
    pci_dev_t *gpu = pci_gpu();

    vram_probe();      /* once only: writing to a BAR is unacceptable on the hot path */

    cur.width  = fb_width();
    cur.height = fb_height();
    cur.bpp    = fb_bpp_get();
    cur.pitch  = fb_pitch_get();
    cur.phys   = fb_base();
    cur.hz     = 60;

    if (bga_detect()) {
        backend = GPU_BGA;
    } else if (svga_detect(gpu)) {
        backend = GPU_VMWARE;
    } else {
        backend = fb_active() ? GPU_FIXED : GPU_NONE;
    }
}

int  vbe_backend(void)  { return backend; }
u16  vbe_bga_version(void) { return bga_version; }

const char *vbe_backend_name(void) {
    switch (backend) {
        case GPU_BGA:    return T("Bochs VBE Extensions (ports 0x1CE/0x1CF)", "Bochs VBE Extensions (порты 0x1CE/0x1CF)");
        case GPU_VMWARE: return T("VMware SVGA II (BAR0 registers)", "VMware SVGA II (регистры BAR0)");
        case GPU_FIXED:  return T("fixed mode from the bootloader", "фиксированный режим от загрузчика");
        default:         return T("not detected", "не обнаружен");
    }
}

int vbe_can_modeset(void) { return backend == GPU_BGA || backend == GPU_VMWARE; }

void vbe_current(vmode_t *m) { *m = cur; }

/* ---------- list of supported modes ---------- */
static const vmode_t mode_table[] = {
    {  640,  480, 32, 0, 0, 60 },
    {  800,  600, 32, 0, 0, 60 },
    { 1024,  768, 32, 0, 0, 60 },
    { 1152,  864, 32, 0, 0, 60 },
    { 1280,  720, 32, 0, 0, 60 },
    { 1366,  768, 32, 0, 0, 60 },   /* native for 14" HD laptops */
    { 1280, 1024, 32, 0, 0, 60 },
    { 1440,  900, 32, 0, 0, 60 },
    { 1600,  900, 32, 0, 0, 60 },
    { 1680, 1050, 32, 0, 0, 60 },
    { 1920, 1080, 32, 0, 0, 60 },
    {  800,  600, 16, 0, 0, 60 },
    { 1024,  768, 16, 0, 0, 60 },
};
#define MODE_COUNT ((u32)(sizeof(mode_table) / sizeof(mode_table[0])))

u32 vbe_mode_count(void) { return MODE_COUNT; }

int vbe_mode_get(u32 i, vmode_t *m) {
    if (i >= MODE_COUNT) return 0;
    *m = mode_table[i];
    return 1;
}

/* Check whether a mode fits into video memory */
int vbe_mode_fits(u32 w, u32 h, u32 bpp) {
    u32 need = w * h * (bpp >> 3);
    u32 have = vbe_vram_bytes();
    return have == 0 || need <= have;
}

/* The amount of video memory.
   IMPORTANT: determining the BAR size requires writing 0xFFFFFFFF into
   the register, which momentarily removes the device's memory
   decoding. Doing that every frame (the GUI mode list is redrawn 30
   times per second) is not allowed - on real hardware it corrupts the
   output. The value is therefore probed once at init and cached. */
static u32 vram_cached = 0;

static void vram_probe(void) {
    pci_dev_t *gpu = pci_gpu();
    vram_cached = 0;
    if (gpu) {
        int idx = -1;
        pci_bar_mem(gpu, &idx);
        if (idx >= 0) vram_cached = pci_bar_size(gpu, idx);
    }
}

u32 vbe_vram_bytes(void) { return vram_cached; }

/* ---------- the mode switch itself ---------- */
int vbe_set_mode(u32 w, u32 h, u32 bpp) {
    if (!vbe_can_modeset()) return VBE_ERR_UNSUPPORTED;
    if (w < 320 || h < 200 || w > 1920 || h > 1200) return VBE_ERR_BADPARAM;
    if (bpp != 32 && bpp != 16 && bpp != 24) return VBE_ERR_BADPARAM;
    if (!vbe_mode_fits(w, h, bpp)) return VBE_ERR_NOVRAM;

    u32 phys = 0, pitch = 0;

    if (backend == GPU_BGA) {
        bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        bga_write(VBE_DISPI_INDEX_XRES, (u16)w);
        bga_write(VBE_DISPI_INDEX_YRES, (u16)h);
        bga_write(VBE_DISPI_INDEX_BPP, (u16)bpp);
        bga_write(VBE_DISPI_INDEX_VIRT_WIDTH, (u16)w);
        bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
        bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        bga_write(VBE_DISPI_INDEX_ENABLE,
                  VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

        /* verify the adapter accepted what was asked */
        if (bga_read(VBE_DISPI_INDEX_XRES) != (u16)w ||
            bga_read(VBE_DISPI_INDEX_YRES) != (u16)h)
            return VBE_ERR_REJECTED;

        pci_dev_t *gpu = pci_gpu();
        if (gpu) { int i = -1; phys = pci_bar_mem(gpu, &i); }
        if (!phys) phys = fb_base();          /* keep the previous address */
        pitch = w * (bpp >> 3);

    } else {  /* VMware SVGA II */
        svga_write(SVGA_REG_ENABLE, 0);
        svga_write(SVGA_REG_WIDTH, w);
        svga_write(SVGA_REG_HEIGHT, h);
        svga_write(SVGA_REG_BPP, bpp);
        svga_write(SVGA_REG_ENABLE, 1);
        svga_write(SVGA_REG_CONFIG_DONE, 1);
        phys  = svga_read(SVGA_REG_FB_START);
        pitch = svga_read(SVGA_REG_BYTES_PER_LINE);
        if (!pitch) pitch = w * (bpp >> 3);
        if (!phys) phys = svga_fb;
    }

    /* rebind the framebuffer driver to the new mode */
    if (fb_remap(phys, w, h, pitch, (u8)bpp) < 0) return VBE_ERR_MAP;

    cur.width = w; cur.height = h; cur.bpp = bpp;
    cur.pitch = pitch; cur.phys = phys;
    return VBE_OK;
}

const char *vbe_error_text(int code) {
    switch (code) {
        case VBE_OK:              return T("success", "успешно");
        case VBE_ERR_UNSUPPORTED: return T("the adapter cannot change mode from the OS", "видеокарта не поддерживает смену режима из ОС");
        case VBE_ERR_BADPARAM:    return T("invalid mode parameters", "недопустимые параметры режима");
        case VBE_ERR_NOVRAM:      return T("not enough video memory for this mode", "не хватает видеопамяти для такого режима");
        case VBE_ERR_REJECTED:    return T("the adapter rejected the requested mode", "видеокарта отклонила запрошенный режим");
        case VBE_ERR_MAP:         return T("could not map the video memory", "не удалось отобразить видеопамять");
        default:                  return T("unknown error", "неизвестная ошибка");
    }
}

/* ============================================================
 *  Screen refresh rate.
 *  We reprogram the CRTC timings: rate = pixel clock / (total width x
 *  total height). Changing the size of the blanking intervals shifts
 *  the frame rate. Works on adapters with a classic CRTC.
 * ============================================================ */

static u32 refresh_hz = 60;

static void crtc_unlock(void) {
    outb(CRTC_INDEX, 0x11);
    u8 v = inb(CRTC_DATA);
    outb(CRTC_DATA, (u8)(v & 0x7F));      /* unprotect registers 0-7 */
}

/* Estimate the current rate from the CRTC registers and the chosen pixel clock */
u32 vbe_measure_hz(void) {
    /* In linear framebuffer mode the CRTC registers do not describe the
       real scan-out - the timings come from the adapter or the host. */
    if (fb_active() && (backend == GPU_BGA || backend == GPU_VMWARE)) return 0;

    u8 misc = inb(VGA_MISC_R);
    u32 pixclk = (misc & 0x0C) == 0 ? 25175000u : 28322000u;

    outb(CRTC_INDEX, 0x00); u32 htotal = (u32)inb(CRTC_DATA) + 5;
    outb(CRTC_INDEX, 0x06); u32 vtotal = inb(CRTC_DATA);
    outb(CRTC_INDEX, 0x07); u8 ovf = inb(CRTC_DATA);
    vtotal |= ((u32)(ovf & 0x01) << 8) | ((u32)(ovf & 0x20) << 4);
    vtotal += 2;

    u32 denom = htotal * 8u * vtotal;
    if (!denom) return 0;
    return pixclk / denom;
}

/* Change the frame rate.

   Changing Vertical Total alone is not allowed: the vertical sync pulse
   would stay in its old place, the signal would become inconsistent and
   the monitor (or emulator) would blank the picture. So the whole block
   of vertical timings is recomputed: Total -> Sync Start -> Sync End,
   keeping the visible part (Vertical Display End) unchanged. */
int vbe_set_refresh(u32 hz) {
    if (hz < 50 || hz > 120) return VBE_ERR_BADPARAM;

    /* On emulated adapters in LFB mode the host dictates the timings:
       the CRTC there is decorative, so we say so honestly. */
    if (backend == GPU_BGA || backend == GPU_VMWARE) {
        if (fb_active()) {
            refresh_hz = hz;
            return VBE_WARN_VIRTUAL;   /* the value is accepted, but the host decides */
        }
    }

    u8 misc = inb(VGA_MISC_R);
    u32 pixclk = (misc & 0x0C) == 0 ? 25175000u : 28322000u;

    outb(CRTC_INDEX, 0x00);
    u32 htotal = (u32)inb(CRTC_DATA) + 5;
    if (!htotal) return VBE_ERR_UNSUPPORTED;

    /* visible frame height in scan lines */
    outb(CRTC_INDEX, 0x12); u32 vde = inb(CRTC_DATA);
    outb(CRTC_INDEX, 0x07); u8 ovf = inb(CRTC_DATA);
    vde |= ((u32)(ovf & 0x02) << 7) | ((u32)(ovf & 0x40) << 3);
    vde += 1;

    u32 vtotal = pixclk / (htotal * 8u * hz);

    /* a frame must hold the visible part plus the blanking interval */
    u32 vmin = vde + 3 + 2 + 8;          /* front porch + sync + back porch */
    if (vtotal < vmin) return VBE_ERR_BADPARAM;
    if (vtotal > 1023) return VBE_ERR_BADPARAM;

    u32 vsync_start = vde + 3;                    /* front porch */
    u32 vsync_end   = vsync_start + 2;            /* pulse width - 2 lines */
    if (vsync_end >= vtotal) return VBE_ERR_BADPARAM;

    u32 fl = irq_save();
    crtc_unlock();

    /* Vertical Total (0x06) + the high bits in Overflow (0x07) */
    u32 vt = vtotal - 2;
    outb(CRTC_INDEX, 0x06);
    outb(CRTC_DATA, (u8)(vt & 0xFF));

    outb(CRTC_INDEX, 0x07);
    u8 o = inb(CRTC_DATA);
    o = (u8)((o & ~0x21) | ((vt >> 8) & 0x01) | (((vt >> 9) & 0x01) << 5));
    /* high bits of Vertical Retrace Start */
    o = (u8)((o & ~0x84) | (((vsync_start >> 8) & 0x01) << 2)
                          | (((vsync_start >> 9) & 0x01) << 7));
    outb(CRTC_DATA, o);

    /* Vertical Retrace Start (0x10) */
    outb(CRTC_INDEX, 0x10);
    outb(CRTC_DATA, (u8)(vsync_start & 0xFF));

    /* Vertical Retrace End (0x11): low 4 bits, the top bit protects 0-7 */
    outb(CRTC_INDEX, 0x11);
    u8 vre = inb(CRTC_DATA);
    vre = (u8)((vre & 0xF0) | (vsync_end & 0x0F));
    outb(CRTC_DATA, vre);

    irq_restore(fl);

    refresh_hz = hz;
    cur.hz = hz;
    return VBE_OK;
}

u32 vbe_get_refresh(void) { return refresh_hz; }

/* Measure the real rate from the vertical blanking signal (port
   0x3DA). Edges are counted over 500 ms of the PIT timer.

   Important: in linear framebuffer mode emulators (QEMU/Bochs) often
   do not model bit 3 of register 0x3DA - it is either always zero or
   toggles on every read. Passing such a result off as a frame rate
   would be meaningless, so the plausibility is checked. */
u32 vbe_count_vsync(void) {
    u32 target = (u32)timer_ticks() + timer_hz() / 2;      /* half a second */
    u32 edges = 0, samples = 0, high = 0;
    int prev = (inb(0x3DA) & 0x08) ? 1 : 0;

    while ((u32)timer_ticks() < target) {
        int now = (inb(0x3DA) & 0x08) ? 1 : 0;
        if (now && !prev) edges++;
        if (now) high++;
        prev = now;
        samples++;
    }
    if (!samples) return 0;

    u32 hz = edges * 2;

    /* The signal never changes state - the scan-out is not emulated */
    if (high == 0 || high == samples) return 0;
    /* Above 300 Hz is physically impossible: the bit is just noisy on every read */
    if (hz > 300) return 0;
    return hz;
}
