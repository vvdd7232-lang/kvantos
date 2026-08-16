/* MTRR: write-combining mode for video memory.
 *
 * Why this is needed. By default, once paging is enabled, the
 * framebuffer is mapped like ordinary memory, but on real hardware the
 * PCI/PCIe region stays uncacheable (UC): EVERY pixel write becomes a
 * separate bus transaction and the CPU waits for it to complete.
 * Copying a 1024x768x32 frame (3 MiB) that way takes tens of
 * milliseconds - hence 10-15 frames per second on a laptop.
 *
 * Write-combining (WC) lets the CPU accumulate writes in a buffer and
 * send them in 64-byte bursts. For linear frame output that is several
 * times faster.
 *
 * Under QEMU there is almost no difference (all memory is ordinary host
 * RAM), so the effect shows up on live hardware.
 */
#include "kernel.h"

#define IA32_MTRRCAP        0x0FE
#define IA32_MTRR_DEF_TYPE  0x2FF
#define IA32_MTRR_PHYSBASE0 0x200
#define MTRR_TYPE_WC        0x01

static int mtrr_ok = 0;
static int mtrr_slot_used = -1;
static u32 mtrr_owned = 0;      /* bitmask of the pairs we programmed */

static inline void rdmsr(u32 msr, u32 *lo, u32 *hi) {
    __asm__ volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void wrmsr(u32 msr, u32 lo, u32 hi) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline void cpuid_raw(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                             : "a"(leaf), "c"(0));
}

/* Check bit 21 (ID) in EFLAGS: without CPUID there are no MSRs either. */
static int has_cpuid(void) {
    u32 res;
    __asm__ volatile(
        "pushfl\n\t"
        "pushfl\n\t"
        "popl %%eax\n\t"
        "movl %%eax, %%ecx\n\t"
        "xorl $0x200000, %%eax\n\t"
        "pushl %%eax\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %%eax\n\t"
        "xorl %%ecx, %%eax\n\t"
        "andl $0x200000, %%eax\n\t"
        "popfl\n\t"
        : "=a"(res) : : "ecx", "cc");
    return res != 0;
}

/* Round the length down to a power of two: MTRRs handle nothing else. */
static u32 pow2_floor(u32 v) {
    u32 p = 1;
    while ((p << 1) && (p << 1) <= v) p <<= 1;
    return p;
}

/* The framebuffer must be aligned to its own size. */
static u32 fit_size(u32 base, u32 size) {
    u32 s = pow2_floor(size);
    while (s >= 0x100000u && (base & (s - 1))) s >>= 1;
    return s;
}

int mtrr_available(void) { return mtrr_ok; }

/* Program one variable MTRR pair. Returns the slot used, or -1. */
static int mtrr_program(u32 vcnt, int slot, u32 base, u32 len) {
    if (slot < 0) {
        for (u32 i = 0; i < vcnt; i++) {
            u32 mlo, mhi;
            rdmsr(IA32_MTRR_PHYSBASE0 + i * 2 + 1, &mlo, &mhi);
            if (!(mlo & (1u << 11))) { slot = (int)i; break; }
        }
    }
    if (slot < 0) return -1;

    u32 fl = irq_save();

    u32 cr0;
    __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movl %0, %%cr0" : : "r"((cr0 | 0x40000000u) & ~0x20000000u));
    __asm__ volatile("wbinvd");

    u32 def_lo, def_hi;
    rdmsr(IA32_MTRR_DEF_TYPE, &def_lo, &def_hi);
    wrmsr(IA32_MTRR_DEF_TYPE, def_lo & ~(1u << 11), def_hi);

    wrmsr(IA32_MTRR_PHYSBASE0 + slot * 2,
          (base & 0xFFFFF000u) | MTRR_TYPE_WC, 0);
    wrmsr(IA32_MTRR_PHYSBASE0 + slot * 2 + 1,
          ((~(len - 1)) & 0xFFFFF000u) | (1u << 11), 0x0000000F);

    wrmsr(IA32_MTRR_DEF_TYPE, def_lo | (1u << 11), def_hi);

    __asm__ volatile("wbinvd");
    __asm__ volatile("movl %0, %%cr0" : : "r"(cr0));

    irq_restore(fl);
    return slot;
}

/* Returns 1 if WC was successfully assigned to the region. */
int mtrr_set_wc(u32 base, u32 size) {
    if (!has_cpuid()) return 0;

    u32 a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    if (!(d & (1u << 12))) return 0;      /* EDX bit 12: MTRR support */
    if (!(d & (1u << 5)))  return 0;      /* bit 5: MSRs present */

    u32 cap_lo, cap_hi;
    rdmsr(IA32_MTRRCAP, &cap_lo, &cap_hi);
    u32 vcnt = cap_lo & 0xFF;             /* number of variable registers */
    if (!(cap_lo & (1u << 10))) return 0; /* bit 10: WC support */
    if (!vcnt) return 0;

    /* A single MTRR can only describe a power-of-two block aligned to its
       own size, so one register never covers a framebuffer whose size is
       not a power of two. At 1920x1080x32 (7.91 MiB) it used to cover
       just 4 MiB: the remaining half of every frame stayed uncacheable,
       and each write there became a separate bus transaction. That is
       what dragged the desktop down to ~20 fps as the resolution grew -
       and it got worse with size, exactly as reported.

       The region is therefore covered with SEVERAL registers: the
       largest fitting block, then the next one for the remainder, and so
       on while free MTRRs and at least 1 MiB of tail remain. */
    int used_any = 0;
    u32 addr = base;
    u32 left = size;

    /* Release the pairs we programmed last time before re-arming, so a
       sequence of mode changes cannot exhaust the registers. */
    for (u32 i = 0; i < vcnt && i < 32; i++) {
        if (!(mtrr_owned & (1u << i))) continue;
        u32 fl = irq_save();
        wrmsr(IA32_MTRR_PHYSBASE0 + i * 2, 0, 0);
        wrmsr(IA32_MTRR_PHYSBASE0 + i * 2 + 1, 0, 0);
        irq_restore(fl);
    }
    mtrr_owned = 0;

    while (left >= 0x100000u) {
        u32 len = fit_size(addr, left);
        if (len < 0x100000u) break;

        int slot = mtrr_program(vcnt, -1, addr, len);
        if (slot < 0) break;                  /* no free registers left */

        mtrr_owned |= (1u << slot);
        mtrr_slot_used = slot;
        used_any = 1;

        addr += len;
        left -= len;
    }

    if (!used_any) return 0;
    mtrr_ok = 1;
    return 1;
}

u32 mtrr_wc_bytes(void) {
    return mtrr_ok ? 1 : 0;
}

int mtrr_slot(void) { return mtrr_slot_used; }
