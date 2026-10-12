/* Paging: identity mapping of the first 128 MiB.
   32-bit build: classic two-level tables with 4 KiB pages.
   64-bit build (KvantOS 3.0): PML4 + 2 MiB pages, the first
   4 MiB mapped 4 KiB-granular so that page zero stays unmapped. */
#include "kernel.h"

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
#define PAGE_PSE     0x80     /* 2 MiB page in a PD entry */


#ifdef __x86_64__
/* ================= the 64-bit implementation ================= */

#define IDENTITY_MB  128

static u64 pml4[512]  __attribute__((aligned(4096)));
static u64 pdpt[512]  __attribute__((aligned(4096)));
static u64 pd[512]    __attribute__((aligned(4096)));
static u64 pt0[512]   __attribute__((aligned(4096)));   /* first 4 MiB, 4K pages */

static void page_fault(registers_t *r) {
    kv_addr_t addr = read_cr2();
    if (kapp_in_app()) {
        static char why[64];
        ksnprintf(why, sizeof(why), T("memory access at address 0x%08x", "обращение к памяти по адресу 0x%08x"), (u32)addr);
        kapp_recover(why);
    }
    kprintf(T("\n[PF] address=%p %s%s%s\n", "\n[PF] адрес=%p %s%s%s\n"), (void *)addr,
            (r->err_code & 1) ? T("protection ", "защита ") : T("not-present ", "не-присутствует "),
            (r->err_code & 2) ? T("write ", "запись ") : T("read ", "чтение "),
            (r->err_code & 4) ? "user" : "kernel");
    panic("Page fault", r);
}

void paging_init(void) {
    /* first 4 MiB: 4 KiB pages, page zero deliberately absent -
       dereferencing NULL must raise an honest exception */
    for (int i = 1; i < 512; i++)
        pt0[i] = ((u64)i << 12) | PAGE_PRESENT | PAGE_RW;
    pt0[0] = 0;

    /* 4 MiB .. 128 MiB: 2 MiB pages */
    for (int i = 1; i < IDENTITY_MB / 2; i++)
        pd[i] = ((u64)i << 21) | PAGE_PRESENT | PAGE_RW | PAGE_PSE;
    pd[0] = (u64)pt0 | PAGE_PRESENT | PAGE_RW;

    pdpt[0] = (u64)pd | PAGE_PRESENT | PAGE_RW;
    pml4[0] = (u64)pdpt | PAGE_PRESENT | PAGE_RW;

    isr_install_handler(14, page_fault);
    paging_enable((kv_addr_t)pml4);
}

/* Walk (and grow) the 4-level tables for a single 4 KiB page.
   If a 2 MiB page already covers the address, it is split. */
static u64 *walk(u64 virt, int alloc) {
    u64 *lvl = pml4;
    for (int l = 3; l >= 1; l--) {
        int idx = (virt >> (12 + 9 * l)) & 0x1FF;
        u64 e = lvl[idx];
        if (!(e & PAGE_PRESENT)) {
            if (!alloc) return NULL;
            u32 frame = pmm_alloc_frame();
            if (!frame) return NULL;
            memset((void *)frame, 0, 4096);
            lvl[idx] = frame | PAGE_PRESENT | PAGE_RW;
            e = lvl[idx];
        }
        if (l == 1 && (e & PAGE_PSE)) {
            /* a 2 MiB page is in the way: split it into 4 KiB */
            u32 frame = pmm_alloc_frame();
            if (!frame) return NULL;
            u64 base = e & ~0x1FFFFFul;
            u64 *nt = (u64 *)frame;
            for (int i = 0; i < 512; i++)
                nt[i] = base + ((u64)i << 12) | PAGE_PRESENT | PAGE_RW;
            lvl[idx] = frame | PAGE_PRESENT | PAGE_RW;
            e = lvl[idx];
        }
        lvl = (u64 *)(e & ~0xFFFull);
    }
    return &lvl[(virt >> 12) & 0x1FF];
}

int paging_map(u32 virt, u32 phys, int rw) {
    u64 *pte = walk(virt, 1);
    if (!pte) return -1;
    *pte = (phys & ~0xFFFu) | PAGE_PRESENT | (rw ? PAGE_RW : 0);
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

/* Identity-map a range of physical addresses (for MMIO/framebuffer) */
int paging_map_range(u32 base, u32 size, int rw) {
    u32 start = base & ~0xFFFu;
    u32 end   = (base + size + 0xFFF) & ~0xFFFu;
    for (u32 a = start; a < end && a >= start; a += 0x1000)
        if (paging_map(a, a, rw) < 0) return -1;
    return 0;
}

u32 paging_phys(u32 virt) {
    u64 *pte = walk(virt, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    return (u32)((*pte & ~0xFFFull) | (virt & 0xFFF));
}

#else
/* ================= the 32-bit implementation ================= */

#define IDENTITY_MB  128
#define PT_COUNT     (IDENTITY_MB)      /* one table = 1024 * 4K = 4 MiB */

static u32 page_directory[1024] __attribute__((aligned(4096)));
static u32 page_tables[IDENTITY_MB / 4][1024] __attribute__((aligned(4096)));

static void page_fault(registers_t *r) {
    u32 addr = read_cr2();
    /* A fault inside an application is no reason to stop the system:
       kill the culprit and return to the shell. Nothing is printed to
       the screen - that would corrupt the graphics mode. */
    if (kapp_in_app()) {
        static char why[64];
        ksnprintf(why, sizeof(why), T("memory access at address 0x%08x", "обращение к памяти по адресу 0x%08x"), addr);
        kapp_recover(why);
    }
    kprintf(T("\n[PF] address=%p %s%s%s\n", "\n[PF] адрес=%p %s%s%s\n"), (void *)addr,
            (r->err_code & 1) ? T("protection ", "защита ") : T("not-present ", "не-присутствует "),
            (r->err_code & 2) ? T("write ", "запись ") : T("read ", "чтение "),
            (r->err_code & 4) ? "user" : "kernel");
    panic("Page fault", r);
}

void paging_init(void) {
    for (int i = 0; i < 1024; i++) page_directory[i] = PAGE_RW;

    u32 addr = 0;
    for (int t = 0; t < IDENTITY_MB / 4; t++) {
        for (int i = 0; i < 1024; i++) {
            page_tables[t][i] = addr | PAGE_PRESENT | PAGE_RW;
            addr += 0x1000;
        }
        page_directory[t] = ((u32)page_tables[t]) | PAGE_PRESENT | PAGE_RW;
    }

    /* Page zero is deliberately NOT mapped: dereferencing NULL must
       raise an honest exception instead of silently trashing the
       interrupt table. The kernel does not need it - it lives at 1 MiB. */
    page_tables[0][0] = 0;

    isr_install_handler(14, page_fault);
    paging_enable((u32)page_directory);
}

int paging_map(u32 virt, u32 phys, int rw) {
    u32 pdi = virt >> 22, pti = (virt >> 12) & 0x3FF;
    if (!(page_directory[pdi] & PAGE_PRESENT)) {
        u32 frame = pmm_alloc_frame();
        if (!frame) return -1;
        memset((void *)frame, 0, 4096);
        page_directory[pdi] = frame | PAGE_PRESENT | PAGE_RW;
    }
    u32 *pt = (u32 *)(page_directory[pdi] & ~0xFFFu);
    pt[pti] = (phys & ~0xFFFu) | PAGE_PRESENT | (rw ? PAGE_RW : 0);
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

/* Identity-map a range of physical addresses (for MMIO/framebuffer) */
int paging_map_range(u32 base, u32 size, int rw) {
    u32 start = base & ~0xFFFu;
    u32 end   = (base + size + 0xFFF) & ~0xFFFu;
    for (u32 a = start; a < end && a >= start; a += 0x1000)
        if (paging_map(a, a, rw) < 0) return -1;
    return 0;
}

u32 paging_phys(u32 virt) {
    u32 pdi = virt >> 22, pti = (virt >> 12) & 0x3FF;
    if (!(page_directory[pdi] & PAGE_PRESENT)) return 0;
    u32 *pt = (u32 *)(page_directory[pdi] & ~0xFFFu);
    if (!(pt[pti] & PAGE_PRESENT)) return 0;
    return (pt[pti] & ~0xFFFu) | (virt & 0xFFF);
}

#endif /* __x86_64__ */
