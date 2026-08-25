/* ============================================================
 *  KvantOS 3.0 - UEFI boot stub (x86-64)
 *
 *  A tiny EFI application written in plain C. It finds the
 *  64-bit kernel on the boot volume, loads the .kapp modules,
 *  sets up a graphics mode, converts the EFI memory map into a
 *  Multiboot-1 style multiboot_info structure, exits the boot
 *  services and jumps into the kernel:
 *
 *      entry(0x2BADB002, address of multiboot_info)
 *
 *  From there KvantOS is the same system on any machine -
 *  including modern hardware without legacy BIOS / MBR support.
 * ============================================================ */
#include "efi/efi.h"

#define MULTIBOOT_MAGIC  0x2BADB002ULL
#define KERNEL_BASE      0x100000ULL       /* kernel is linked at 1 MiB */
#define MAX_MODULES      16
#define META_PAGES       4                 /* multiboot_info + tables */

/* ---- multiboot-1 compatible layout (matches include/kernel.h) ---- */
typedef struct {
    u32 flags;
    u32 mem_lower, mem_upper;
    u32 boot_device;
    u32 cmdline;
    u32 mods_count, mods_addr;
    u32 syms[4];
    u32 mmap_length, mmap_addr;
    u32 drives_length, drives_addr;
    u32 config_table;
    u32 boot_loader_name;
    u32 apm_table;
    u32 vbe_control_info, vbe_mode_info;
    u16 vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    u32 framebuffer_addr_low, framebuffer_addr_high;
    u32 framebuffer_pitch;
    u32 framebuffer_width, framebuffer_height;
    u8  framebuffer_bpp;
    u8  framebuffer_type;
    u8  fb_red_position, fb_red_mask_size;
    u8  fb_green_position, fb_green_mask_size;
    u8  fb_blue_position, fb_blue_mask_size;
} __attribute__((packed)) mb_info_t;

typedef struct { u32 start, end, string, reserved; } mb_module_t;
typedef struct { u32 size; u64 addr; u64 len; u32 type; } __attribute__((packed)) mb_mmap_t;

/* ---- ELF64 ---- */
typedef struct {
    u8  e_ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} elf64_phdr_t;

#define PT_LOAD 1

/* ---- reserved regions that must survive into the kernel ---- */
static struct { u64 base, end; } reserved[24];
static int reserved_count = 0;

static void reserve(u64 base, u64 end) {
    if (reserved_count < 24 && end > base) {
        reserved[reserved_count].base = base;
        reserved[reserved_count].end = end;
        reserved_count++;
    }
}

static efi_system_table_t  *gST;
static efi_boot_services_t *gBS;
static efi_handle_t         gImage;

/* ===================== tiny helpers ===================== */
static void *memsetb(void *d, int c, u64 n) {
    u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}

static void *memcpyb(void *d, const void *s, u64 n) {
    u8 *dp = d; const u8 *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

static u64 str_len(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

/* char16 -> ASCII (file names we care about are plain ASCII) */
static void to8(const efi_char16_t *s, char *d, u64 max) {
    u64 i = 0;
    while (s[i] && i + 1 < max) { d[i] = (char)(s[i] & 0xFF); i++; }
    d[i] = 0;
}

static void print16(const efi_char16_t *s) {
    if (gST && gST->con_out) gST->con_out->output_string(gST->con_out, (efi_char16_t *)s);
}

static void print(const char *s) {
    efi_char16_t buf[128];
    while (*s) {
        u64 i = 0;
        while (*s && i < 127) {
            if (*s == '\n') { buf[i++] = 13; if (i < 127) buf[i++] = 10; s++; break; }
            buf[i++] = (efi_char16_t)*s++;
        }
        buf[i] = 0;
        print16(buf);
    }
}

static __attribute__((noreturn)) void fail(efi_status_t rc, const char *where) {
    char hex[19];
    hex[0] = '0'; hex[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int d = (int)(rc & 0xF);
        hex[2 + (15 - i)] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        rc >>= 4;
    }
    hex[18] = 0;
    print("\nKvantEFI: ");
    print(where);
    print(" failed (");
    print(hex);
    print(")\nSystem halted by the boot stub.\n");
    for (;;) __asm__ volatile("cli; hlt");
}

/* Allocate pages below 4 GiB: the 64-bit kernel still keeps every
   physical address in 32 bits, exactly like the 32-bit build. */
static u64 alloc_pages_below4g(u64 pages) {
    u64 addr = 0xFFFFFFFFULL;
    efi_status_t s = gBS->allocate_pages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERR(s)) return 0;
    return addr;
}

/* ===================== files ===================== */
static efi_status_t open_path(efi_file_t *root, efi_char16_t *path, efi_file_t **out) {
    return root->open(root, out, path, EFI_FILE_MODE_READ, 0);
}

static efi_status_t file_size(efi_file_t *f, u64 *size) {
    u8 info_buf[1024];
    u64 info_len = sizeof(info_buf);
    efi_guid_t info_id = EFI_FILE_INFO_ID;
    efi_status_t s = f->get_info(f, &info_id, &info_len, info_buf);
    if (EFI_ERR(s)) return s;
    *size = ((efi_file_info_t *)info_buf)->file_size;
    return EFI_SUCCESS;
}

/* Read a whole file into freshly allocated pages (below 4 GiB). */
static efi_status_t read_file(efi_file_t *dir, efi_char16_t *name,
                              u64 *out_addr, u64 *out_size) {
    efi_file_t *f;
    efi_status_t s = dir->open(dir, &f, name, EFI_FILE_MODE_READ, 0);
    if (EFI_ERR(s)) return s;

    u64 size = 0;
    s = file_size(f, &size);
    if (EFI_ERR(s)) { f->close(f); return s; }

    u64 pages = (size + 4095) >> 12;
    if (!pages) pages = 1;
    u64 addr = alloc_pages_below4g(pages);
    if (!addr) { f->close(f); return EFI_OUT_OF_RESOURCES; }

    u64 got = 0;
    while (got < size) {
        u64 chunk = size - got;
        s = f->read(f, &chunk, (void *)(addr + got));
        if (EFI_ERR(s) || chunk == 0) { f->close(f); return EFI_ERR(s) ? s : EFI_LOAD_ERROR; }
        got += chunk;
    }
    f->close(f);
    *out_addr = addr;
    *out_size = size;
    return EFI_SUCCESS;
}

/* ===================== graphics ===================== */
static const efi_guid_t gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const efi_guid_t fs_guid  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static const efi_guid_t li_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;

/* Pick the best text-friendly mode: exactly 1024x768 if the firmware
   offers it, otherwise the largest mode up to 1920x1080. */
static void setup_gop(mb_info_t *mbi) {
    u64 hsize = 0;
    if (EFI_ERR(gBS->locate_handle(1 /* ByProtocol */, (efi_guid_t *)&gop_guid,
                                   0, &hsize, 0))) return;
    if (!hsize) return;
    u64 hcap = hsize + 8 * sizeof(efi_handle_t);
    u64 hbuf = alloc_pages_below4g((hcap + 4095) >> 12);
    if (!hbuf) return;
    hsize = hcap;
    if (EFI_ERR(gBS->locate_handle(1, (efi_guid_t *)&gop_guid, 0, &hsize,
                                   (efi_handle_t *)hbuf))) return;

    efi_graphics_output_t *gop = 0;
    int count = (int)(hsize >> 3);
    for (int i = 0; i < count && !gop; i++) {
        void *proto = 0;
        if (!EFI_ERR(gBS->handle_protocol(((efi_handle_t *)hbuf)[i],
                                          (efi_guid_t *)&gop_guid, &proto)))
            gop = proto;
    }
    if (!gop || !gop->mode || !gop->mode->max_mode) return;

    u32 best_mode = 0xFFFFFFFF;
    u32 best_area = 0;
    u32 best_w = 0, best_h = 0;
    for (u32 m = 0; m < gop->mode->max_mode; m++) {
        u64 isz = 0;
        efi_graphics_output_mode_info_t *info = 0;
        if (EFI_ERR(gop->query_mode(gop, m, &isz, &info))) continue;
        u32 w = info->horizontal_resolution, h = info->vertical_resolution;
        int fmt_ok = info->pixel_format == PixelRedGreenBlueReserved8BitPerColor ||
                     info->pixel_format == PixelBlueGreenRedReserved8BitPerColor;
        if (fmt_ok && w == 1024 && h == 768) {
            best_mode = m; best_w = w; best_h = h; best_area = w * h;
            break;
        }
        if (fmt_ok && w <= 1920 && h <= 1200 && w * h > best_area) {
            best_mode = m; best_w = w; best_h = h; best_area = w * h;
        }
    }
    if (best_mode == 0xFFFFFFFF) return;
    if (EFI_ERR(gop->set_mode(gop, best_mode))) return;

    efi_graphics_output_mode_info_t *info = gop->mode->info;
    u64 fb = gop->mode->frame_buffer_base;

    mbi->framebuffer_addr_low  = (u32)fb;
    mbi->framebuffer_addr_high = (u32)(fb >> 32);
    mbi->framebuffer_pitch     = info->pixels_per_scan_line ?
                                 info->pixels_per_scan_line * 4 : best_w * 4;
    mbi->framebuffer_width  = best_w;
    mbi->framebuffer_height = best_h;
    mbi->framebuffer_bpp    = 32;
    mbi->framebuffer_type   = 1;   /* direct RGB */
    if (info->pixel_format == PixelBlueGreenRedReserved8BitPerColor) {
        mbi->fb_red_position = 16; mbi->fb_green_position = 8; mbi->fb_blue_position = 0;
    } else {
        mbi->fb_red_position = 0;  mbi->fb_green_position = 8; mbi->fb_blue_position = 16;
    }
    mbi->fb_red_mask_size = mbi->fb_green_mask_size = mbi->fb_blue_mask_size = 8;
    mbi->flags |= (1u << 12);
}

/* ===================== memory map ===================== */
static u64 map_buf = 0;
static u64 map_pages = 0;

/* Fetch the EFI memory map into a self-growing buffer. Returns the key. */
static efi_status_t fetch_memory_map(u64 *map_size, u64 *key,
                                     u64 *desc_size, u32 *desc_version) {
    for (;;) {
        u64 size = 0;
        efi_status_t s = gBS->get_memory_map(&size, 0, key, desc_size, desc_version);
        if (s != EFI_BUFFER_TOO_SMALL && EFI_ERR(s)) return s;
        size += *desc_size * 8;                      /* slack for our own growth */
        u64 need = (size + 4095) >> 12;
        if (need > map_pages) {
            u64 nb = alloc_pages_below4g(need);
            if (!nb) return EFI_OUT_OF_RESOURCES;
            map_buf = nb;
            map_pages = need;
        }
        size = map_pages << 12;
        s = gBS->get_memory_map(&size, (efi_memory_descriptor_t *)map_buf,
                                key, desc_size, desc_version);
        if (!EFI_ERR(s)) { *map_size = size; return EFI_SUCCESS; }
        if (s != EFI_BUFFER_TOO_SMALL) return s;
        map_pages += 4;                              /* still too small: grow */
    }
}

static int mb_type_for(u32 efi_type) {
    switch (efi_type) {
    case EfiLoaderCode: case EfiLoaderData:
    case EfiBootServicesCode: case EfiBootServicesData:
    case EfiConventionalMemory:
        return 1;                                    /* available */
    case EfiACPIReclaimMemory:
        return 3;
    case EfiACPIMemoryNVS:
        return 4;
    default:
        return 2;                                    /* reserved */
    }
}

/* Check that [base,end) lies inside an available EFI region. */
static int range_available(u64 map_size, u64 desc_size, u64 base, u64 end) {
    for (u64 off = 0; off < map_size; off += desc_size) {
        efi_memory_descriptor_t *d = (efi_memory_descriptor_t *)(map_buf + off);
        u64 ds = d->phys_start, de = ds + (d->pages << 12);
        if (base >= ds && end <= de && mb_type_for(d->type) == 1) return 1;
    }
    return 0;
}

/* Convert the EFI map into the Multiboot-1 mmap, clipping out the
   regions the kernel must keep (kernel image, modules, tables). */
static u64 build_mb_mmap(mb_mmap_t *out, u64 max_entries, u64 map_size,
                         u64 desc_size, u32 *mem_upper_kb) {
    u64 n = 0;
    u64 top = 0;

    for (u64 off = 0; off < map_size && n + 3 < max_entries; off += desc_size) {
        efi_memory_descriptor_t *d = (efi_memory_descriptor_t *)(map_buf + off);
        u64 base = d->phys_start;
        u64 end  = base + (d->pages << 12);
        if (base >= 0x100000000ULL || end <= base) continue;
        if (end > 0x100000000ULL) end = 0x100000000ULL;

        int t = mb_type_for(d->type);
        if (t != 1) {
            out[n].size = 20; out[n].addr = base; out[n].len = end - base;
            out[n].type = (u32)t;
            n++;
            continue;
        }

        /* available region: emit the fragments not covered by our
           reserved ranges, and the reserved pieces as type 2 */
        u64 cur = base;
        for (int r = 0; r < reserved_count && cur < end; r++) {
            u64 rb = reserved[r].base, re = reserved[r].end;
            if (re <= cur || rb >= end) continue;
            if (rb > cur) {
                out[n].size = 20; out[n].addr = cur; out[n].len = rb - cur;
                out[n].type = 1; n++;
            }
            u64 seg_end = re < end ? re : end;
            if (seg_end > rb) {
                out[n].size = 20; out[n].addr = rb; out[n].len = seg_end - rb;
                out[n].type = 2; n++;
            }
            cur = seg_end;
        }
        if (cur < end) {
            out[n].size = 20; out[n].addr = cur; out[n].len = end - cur;
            out[n].type = 1; n++;
        }
    }

    for (u64 i = 0; i < n; i++) {
        if (out[i].type == 1) {
            u64 e = out[i].addr + out[i].len;
            if (e > top) top = e;
        }
    }
    if (top > 0x100000ULL) *mem_upper_kb = (u32)((top - 0x100000ULL) >> 10);
    else *mem_upper_kb = 0;
    return n;
}

/* ===================== kernel loading ===================== */
static u64 load_kernel(u64 file_addr, u64 file_size) {
    if (file_size < sizeof(elf64_ehdr_t)) return 0;
    elf64_ehdr_t *eh = (elf64_ehdr_t *)file_addr;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return 0;
    if (eh->e_ident[4] != 2 || eh->e_machine != 62) return 0;   /* 64-bit x86-64 */

    u64 max_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(file_addr + eh->e_phoff +
                                            (u64)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        if (ph->p_paddr != ph->p_vaddr) continue;     /* identity kernel */

        u64 dst = ph->p_vaddr;
        if (dst < KERNEL_BASE) return 0;              /* refuses to clobber low memory */
        if (ph->p_offset + ph->p_filesz > file_size) return 0;
        memcpyb((void *)dst, (void *)(file_addr + ph->p_offset), ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memsetb((void *)(dst + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);

        u64 end = (dst + ph->p_memsz + 4095) & ~4095ULL;
        if (end > max_end) max_end = end;
    }
    if (!max_end || !eh->e_entry) return 0;
    reserve(KERNEL_BASE, max_end);
    return eh->e_entry;
}

static int ends_with_kapp(const char *name) {
    u64 n = str_len(name);
    if (n < 5) return 0;
    const char *s = name + n - 5;
    return (s[0] == '.') &&
           (s[1] == 'k' || s[1] == 'K') && (s[2] == 'a' || s[2] == 'A') &&
           (s[3] == 'p' || s[3] == 'P') && (s[4] == 'p' || s[4] == 'P');
}

/* Load /boot/apps/ *.kapp as Multiboot modules. The kernel copies them
   into ramfs during boot and shows them in the application launcher. */
static int load_modules(efi_file_t *root, mb_module_t *mods, char *names) {
    efi_file_t *dir;
    efi_status_t s = open_path(root, (efi_char16_t *)u"boot\\apps", &dir);
    if (EFI_ERR(s)) return 0;

    int count = 0;
    u8 buf[2048];
    while (count < MAX_MODULES) {
        u64 blen = sizeof(buf);
        s = dir->read(dir, &blen, buf);
        if (EFI_ERR(s) || blen == 0) break;

        for (u64 off = 0; off + sizeof(efi_file_info_t) <= blen;) {
            efi_file_info_t *fi = (efi_file_info_t *)(buf + off);
            char name[64];
            to8(fi->file_name, name, sizeof(name));

            if (!(fi->attribute & EFI_FILE_DIRECTORY) && ends_with_kapp(name)) {
                u64 addr = 0, size = 0;
                if (!EFI_ERR(read_file(dir, fi->file_name, &addr, &size)) && size) {
                    u64 end = (addr + size + 4095) & ~4095ULL;
                    reserve(addr, end);

                    char *nm = names + count * 64;
                    nm[0] = '/';
                    to8(fi->file_name, nm + 1, 63);

                    mods[count].start = (u32)addr;
                    mods[count].end = (u32)(addr + size);
                    mods[count].string = 0;       /* patched once meta address is final */
                    mods[count].reserved = 0;
                    count++;
                }
            }
            off += fi->size ? fi->size : sizeof(efi_file_info_t);
        }
    }
    dir->close(dir);
    return count;
}

/* ===================== entry ===================== */
efi_status_t EFIAPI efi_main(efi_handle_t image, efi_system_table_t *systab) {
    gST = systab;
    gBS = systab->boot_services;
    gImage = image;

    print("KvantOS 3.0 UEFI loader (x86-64)\n");

    if (!gBS) fail(EFI_UNSUPPORTED, "no boot services");

    /* --- where do we boot from? --- */
    efi_loaded_image_t *li = 0;
    if (EFI_ERR(gBS->handle_protocol(image, (efi_guid_t *)&li_guid, (void **)&li)))
        fail(EFI_LOAD_ERROR, "loaded image protocol");
    efi_simple_file_system_t *fs = 0;
    if (EFI_ERR(gBS->handle_protocol(li->device_handle, (efi_guid_t *)&fs_guid, (void **)&fs)))
        fail(EFI_LOAD_ERROR, "no filesystem on the boot device");
    efi_file_t *root = 0;
    if (EFI_ERR(fs->open_volume(fs, &root)))
        fail(EFI_LOAD_ERROR, "cannot open the boot volume");

    /* --- the kernel --- */
    print("Loading /boot/kvant64.bin ... ");
    u64 kfile = 0, ksize = 0;
    if (EFI_ERR(read_file(root, (efi_char16_t *)u"boot\\kvant64.bin", &kfile, &ksize)))
        fail(EFI_NOT_FOUND, "/boot/kvant64.bin");

    u64 map_size = 0, map_key = 0, desc_size = 0;
    u32 desc_version = 0;
    if (EFI_ERR(fetch_memory_map(&map_size, &map_key, &desc_size, &desc_version)))
        fail(EFI_LOAD_ERROR, "memory map");

    /* The kernel lives in the 1..16 MiB window; refuse to clobber
       anything if the firmware says the window is not free RAM. */
    if (!range_available(map_size, desc_size, KERNEL_BASE, 0x1000000ULL))
        fail(EFI_LOAD_ERROR, "the 1-16 MiB window is not available RAM");

    u64 entry = load_kernel(kfile, ksize);
    if (!entry) fail(EFI_LOAD_ERROR, "kvant64.bin is not a valid ELF64 kernel");
    print("ok\n");

    /* --- meta area: multiboot_info + module table + mmap --- */
    u64 meta = alloc_pages_below4g(META_PAGES);
    if (!meta) fail(EFI_OUT_OF_RESOURCES, "meta area");
    reserve(meta, meta + META_PAGES * 4096);
    memsetb((void *)meta, 0, META_PAGES * 4096);

    mb_info_t   *mbi     = (mb_info_t *)meta;
    mb_module_t *mods    = (mb_module_t *)(meta + 0x100);
    char        *cmdline = (char *)(meta + 0x400);
    char        *loader  = (char *)(meta + 0x440);
    char        *names   = (char *)(meta + 0x800);
    mb_mmap_t   *mmap    = (mb_mmap_t *)(meta + 0x1000);

    /* --- applications --- */
    int nmods = load_modules(root, mods, names);
    mbi->mods_count = (u32)nmods;
    mbi->mods_addr  = (u32)(meta + 0x100);
    for (int i = 0; i < nmods; i++)
        mods[i].string = (u32)(meta + 0x800 + i * 64);
    if (nmods) mbi->flags |= (1u << 3);

    /* --- graphics --- */
    setup_gop(mbi);

    /* --- strings --- */
    const char *cl = "kvantefi uefi";
    const char *ln = "KvantEFI 3.0 (UEFI x86_64)";
    memcpyb(cmdline, cl, str_len(cl) + 1);
    memcpyb(loader, ln, str_len(ln) + 1);
    mbi->cmdline = (u32)(meta + 0x400);
    mbi->flags |= (1u << 2);
    mbi->boot_loader_name = (u32)(meta + 0x440);
    mbi->flags |= (1u << 9);

    /* --- memory map, fresh fetch right before handing over --- */
    map_size = 0;
    for (;;) {
        efi_status_t s = fetch_memory_map(&map_size, &map_key, &desc_size, &desc_version);
        if (EFI_ERR(s)) fail(s, "memory map (final)");

        u32 mem_upper_kb = 0;
        u64 n = build_mb_mmap(mmap, 512, map_size, desc_size, &mem_upper_kb);
        mbi->mmap_addr = (u32)(meta + 0x1000);
        mbi->mmap_length = (u32)(n * sizeof(mb_mmap_t));
        mbi->flags |= (1u << 6);
        mbi->mem_lower = 640;
        mbi->mem_upper = mem_upper_kb;
        mbi->flags |= 1;

        s = gBS->exit_boot_services(image, map_key);
        if (!EFI_ERR(s)) break;
        /* the key went stale: map changed, try again with the new one */
    }

    print("Booting KvantOS 3.0 (64-bit)...\n");

    void (*start)(u64, u64) = (void (*)(u64, u64))entry;
    start(MULTIBOOT_MAGIC, meta);

    /* the kernel never returns */
    for (;;) __asm__ volatile("cli; hlt");
    __builtin_unreachable();
}
