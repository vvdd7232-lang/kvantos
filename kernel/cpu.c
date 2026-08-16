/* CPUID, перезагрузка и выключение */
#include "kernel.h"

static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* На 386/486 инструкции cpuid нет: её выполнение выдаёт #UD и роняет
   ядро сразу после старта. Наличие определяется по возможности
   переключить бит 21 (ID) в EFLAGS. */
static int cpuid_supported(void) {
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

void cpu_vendor(char *buf13) {
    u32 a, b, c, d;
    if (!cpuid_supported()) { strcpy(buf13, "i386/i486"); return; }
    cpuid(0, &a, &b, &c, &d);
    ((u32 *)buf13)[0] = b;
    ((u32 *)buf13)[1] = d;
    ((u32 *)buf13)[2] = c;
    buf13[12] = 0;
}

void cpu_brand(char *buf49) {
    u32 a, b, c, d;
    if (!cpuid_supported()) { strcpy(buf49, "процессор без CPUID (386/486)"); return; }
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x80000004) { strcpy(buf49, "неизвестный процессор"); return; }
    u32 *p = (u32 *)buf49;
    for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        cpuid(leaf, &a, &b, &c, &d);
        *p++ = a; *p++ = b; *p++ = c; *p++ = d;
    }
    buf49[48] = 0;
}

void kv_reboot(void) {
    u8 st = 0x02;
    while (st & 0x02) st = inb(0x64);
    outb(0x64, 0xFE);          /* импульс сброса через контроллер клавиатуры */
    cli();
    for (;;) hlt();
}

void kv_poweroff(void) {
    /* ACPI-выключение QEMU/Bochs */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    kprintf("\nВыключение не поддерживается этой машиной. Останов CPU.\n");
    cli();
    for (;;) hlt();
}
