/* ============================================================
 *  KvantOS - PCI bus: device enumeration, BAR reading
 *  Configuration access through ports 0xCF8 / 0xCFC.
 * ============================================================ */
#include "kernel.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static pci_dev_t devices[PCI_MAX_DEV];
static u32 dev_count = 0;
static int gpu_index = -1;

u32 pci_read32(u8 bus, u8 slot, u8 func, u8 off) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)slot << 11) |
               ((u32)func << 8) | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

void pci_write32(u8 bus, u8 slot, u8 func, u8 off, u32 val) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)slot << 11) |
               ((u32)func << 8) | (off & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, val);
}

u16 pci_read16(u8 bus, u8 slot, u8 func, u8 off) {
    return (u16)((pci_read32(bus, slot, func, off) >> ((off & 2) * 8)) & 0xFFFF);
}

u8 pci_read8(u8 bus, u8 slot, u8 func, u8 off) {
    return (u8)((pci_read32(bus, slot, func, off) >> ((off & 3) * 8)) & 0xFF);
}

const char *pci_vendor_name(u16 vid) {
    switch (vid) {
        case 0x8086: return "Intel";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD/ATI";
        case 0x1234: return "QEMU/Bochs";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x1B36: return "Red Hat (QXL)";
        case 0x1013: return "Cirrus Logic";
        case 0x5333: return "S3 Graphics";
        case 0x1414: return "Microsoft";
        case 0x106B: return "Apple";
        case 0x1022: return "AMD";
        case 0x1179: return "Toshiba";
        case 0x10EC: return "Realtek";
        case 0x111D: return "IDT";
        case 0x1274: return "Ensoniq";
        default: return T("unknown", "неизвестный");
    }
}

const char *pci_class_name(u8 cls, u8 sub) {
    switch (cls) {
        case 0x00: return T("pre-classification device", "устройство до классификации");
        case 0x01:
            switch (sub) {
                case 0x01: return T("IDE controller", "IDE-контроллер");
                case 0x06: return T("SATA controller", "SATA-контроллер");
                case 0x08: return T("NVMe controller", "NVMe-контроллер");
                default: return T("storage controller", "контроллер накопителей");
            }
        case 0x02: return T("network controller", "сетевой контроллер");
        case 0x03:
            switch (sub) {
                case 0x00: return T("VGA-compatible display adapter", "VGA-совместимый видеоадаптер");
                case 0x01: return T("XGA display adapter", "XGA-видеоадаптер");
                case 0x02: return T("3D display adapter", "3D-видеоадаптер");
                default: return T("display adapter", "видеоадаптер");
            }
        case 0x04: return T("multimedia", "мультимедиа");
        case 0x05: return T("memory controller", "контроллер памяти");
        case 0x06:
            switch (sub) {
                case 0x00: return T("host bridge", "мост процессор-шина");
                case 0x01: return T("PCI-ISA bridge", "мост PCI-ISA");
                case 0x04: return T("PCI-PCI bridge", "мост PCI-PCI");
                default: return T("bridge", "мост");
            }
        case 0x07: return T("communication controller", "контроллер связи");
        case 0x08: return T("system device", "системное устройство");
        case 0x09: return T("input device", "устройство ввода");
        case 0x0C:
            switch (sub) {
                case 0x03: return T("USB controller", "USB-контроллер");
                default: return T("serial bus", "последовательная шина");
            }
        case 0x0D: return T("wireless controller", "беспроводной контроллер");
        default: return T("other device", "прочее устройство");
    }
}

/* Friendly name for well-known display adapters */
const char *pci_gpu_model(u16 vid, u16 did) {
    if (vid == 0x1234 && did == 0x1111) return "QEMU Standard VGA (Bochs VBE)";
    if (vid == 0x1B36 && did == 0x0100) return "QXL paravirtual GPU";
    if (vid == 0x15AD && did == 0x0405) return "VMware SVGA II";
    if (vid == 0x80EE && did == 0xBEEF) return "VirtualBox Graphics Adapter";
    if (vid == 0x1013 && did == 0x00B8) return "Cirrus Logic GD5446";
    if (vid == 0x1AF4 && did == 0x1050) return "virtio-gpu";
    if (vid == 0x8086) return "Intel Graphics";
    if (vid == 0x10DE) return "NVIDIA GPU";
    if (vid == 0x1002) return "AMD Radeon";
    return T("display adapter", "видеоадаптер");
}

static void probe_function(u8 bus, u8 slot, u8 func) {
    u32 id = pci_read32(bus, slot, func, 0x00);
    u16 vid = (u16)(id & 0xFFFF);
    if (vid == 0xFFFF || vid == 0x0000) return;
    if (dev_count >= PCI_MAX_DEV) return;

    pci_dev_t *d = &devices[dev_count];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor = vid;
    d->device = (u16)(id >> 16);

    u32 cls = pci_read32(bus, slot, func, 0x08);
    d->revision  = (u8)(cls & 0xFF);
    d->prog_if   = (u8)((cls >> 8) & 0xFF);
    d->subclass  = (u8)((cls >> 16) & 0xFF);
    d->class_code = (u8)((cls >> 24) & 0xFF);

    u32 hdr = pci_read32(bus, slot, func, 0x0C);
    d->header_type = (u8)((hdr >> 16) & 0xFF);
    d->irq = pci_read8(bus, slot, func, 0x3C);

    /* base address registers */
    for (int i = 0; i < 6; i++) {
        u32 bar = pci_read32(bus, slot, func, (u8)(0x10 + i * 4));
        d->bar[i] = bar;
        d->bar_is_io[i] = (u8)(bar & 1);
    }

    if (d->class_code == 0x03 && gpu_index < 0) gpu_index = (int)dev_count;
    dev_count++;
}

static void probe_slot(u8 bus, u8 slot) {
    u32 id = pci_read32(bus, slot, 0, 0x00);
    if ((id & 0xFFFF) == 0xFFFF) return;
    probe_function(bus, slot, 0);

    u8 hdr = pci_read8(bus, slot, 0, 0x0E);
    if (hdr & 0x80)                         /* multi-function device */
        for (u8 f = 1; f < 8; f++)
            probe_function(bus, slot, f);
}

void pci_init(void) {
    dev_count = 0;
    gpu_index = -1;
    /* brute force: 256 buses x 32 slots */
    for (u32 bus = 0; bus < 256; bus++)
        for (u8 slot = 0; slot < 32; slot++)
            probe_slot((u8)bus, slot);
}

u32 pci_count(void) { return dev_count; }
pci_dev_t *pci_get(u32 i) { return i < dev_count ? &devices[i] : NULL; }
pci_dev_t *pci_gpu(void) { return gpu_index >= 0 ? &devices[gpu_index] : NULL; }

/* Find the first memory BAR (a candidate for the linear framebuffer) */
u32 pci_bar_mem(const pci_dev_t *d, int *which) {
    for (int i = 0; i < 6; i++) {
        if (d->bar_is_io[i]) continue;
        u32 a = d->bar[i] & 0xFFFFFFF0u;
        if (a) { if (which) *which = i; return a; }
    }
    if (which) *which = -1;
    return 0;
}

/* Determine the BAR size: write all ones, read the mask, restore it */
u32 pci_bar_size(pci_dev_t *d, int idx) {
    u8 off = (u8)(0x10 + idx * 4);
    u32 orig = pci_read32(d->bus, d->slot, d->func, off);
    pci_write32(d->bus, d->slot, d->func, off, 0xFFFFFFFFu);
    u32 mask = pci_read32(d->bus, d->slot, d->func, off);
    pci_write32(d->bus, d->slot, d->func, off, orig);
    if (!mask || mask == 0xFFFFFFFFu) return 0;
    mask &= (orig & 1) ? 0xFFFFFFFCu : 0xFFFFFFF0u;
    return (~mask) + 1;
}

/* Enable memory decoding and bus mastering on the device */
void pci_enable_device(pci_dev_t *d) {
    u32 cmd = pci_read32(d->bus, d->slot, d->func, 0x04);
    cmd |= 0x07;            /* I/O space | memory space | bus master */
    pci_write32(d->bus, d->slot, d->func, 0x04, cmd);
}
