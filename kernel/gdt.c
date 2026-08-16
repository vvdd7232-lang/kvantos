/* Глобальная таблица дескрипторов + TSS */
#include "kernel.h"

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  gran;
    u8  base_high;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

struct tss_entry {
    u32 prev_tss, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr   gp;
static struct tss_entry tss;

extern u32 kernel_stack_top;

static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[i].base_low  = (u16)(base & 0xFFFF);
    gdt[i].base_mid  = (u8)((base >> 16) & 0xFF);
    gdt[i].base_high = (u8)((base >> 24) & 0xFF);
    gdt[i].limit_low = (u16)(limit & 0xFFFF);
    gdt[i].gran      = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access    = access;
}

void set_kernel_stack(u32 esp0) { tss.esp0 = esp0; }

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (u32)&gdt;

    gdt_set(0, 0, 0, 0, 0);                    /* нулевой */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);        /* код ядра   0x08 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);        /* данные ядра 0x10 */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);        /* код user   0x18 */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);        /* данные user 0x20 */

    memset(&tss, 0, sizeof(tss));
    tss.ss0  = 0x10;
    tss.esp0 = (u32)&kernel_stack_top;
    tss.iomap_base = sizeof(tss);
    gdt_set(5, (u32)&tss, sizeof(tss) - 1, 0xE9, 0x00);  /* TSS 0x28 */

    gdt_flush((u32)&gp);
    tss_flush();
}
