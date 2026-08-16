/* MTRR: режим write-combining для видеопамяти.
 *
 * Зачем это нужно. По умолчанию после включения страничной адресации
 * фреймбуфер отображается как обычная память, но на реальном железе
 * область PCI/PCIe остаётся некешируемой (UC): КАЖДАЯ запись в пиксель
 * уходит отдельной транзакцией по шине и процессор ждёт её завершения.
 * Копирование кадра 1024x768x32 (3 МиБ) в таком режиме занимает десятки
 * миллисекунд - отсюда 10-15 кадров в секунду на ноутбуке.
 *
 * Режим write-combining (WC) разрешает процессору накапливать записи в
 * буфере и отправлять их пачками по 64 байта. Для линейного вывода
 * кадра это даёт многократное ускорение.
 *
 * В QEMU разницы почти нет (вся память - обычное ОЗУ хоста), поэтому
 * эффект проявляется именно на живом железе.
 */
#include "kernel.h"

#define IA32_MTRRCAP        0x0FE
#define IA32_MTRR_DEF_TYPE  0x2FF
#define IA32_MTRR_PHYSBASE0 0x200
#define MTRR_TYPE_WC        0x01

static int mtrr_ok = 0;
static int mtrr_slot_used = -1;

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

/* Проверяем бит 21 (ID) в EFLAGS: без CPUID нет и MSR. */
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

/* Округляем длину вниз до степени двойки: MTRR умеет только такие. */
static u32 pow2_floor(u32 v) {
    u32 p = 1;
    while ((p << 1) && (p << 1) <= v) p <<= 1;
    return p;
}

/* Кадровый буфер должен быть выровнен на собственный размер. */
static u32 fit_size(u32 base, u32 size) {
    u32 s = pow2_floor(size);
    while (s >= 0x100000u && (base & (s - 1))) s >>= 1;
    return s;
}

int mtrr_available(void) { return mtrr_ok; }

/* Возвращает 1, если удалось назначить WC на область. */
int mtrr_set_wc(u32 base, u32 size) {
    if (!has_cpuid()) return 0;

    u32 a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    if (!(d & (1u << 12))) return 0;      /* бит 12 EDX: поддержка MTRR */
    if (!(d & (1u << 5)))  return 0;      /* бит 5: наличие MSR */

    u32 cap_lo, cap_hi;
    rdmsr(IA32_MTRRCAP, &cap_lo, &cap_hi);
    u32 vcnt = cap_lo & 0xFF;             /* число переменных регистров */
    if (!(cap_lo & (1u << 10))) return 0; /* бит 10: поддержка WC */
    if (!vcnt) return 0;

    u32 len = fit_size(base, size);
    if (len < 0x100000u) return 0;        /* меньше 1 МиБ смысла нет */

    /* Ищем свободную пару регистров (valid = 0 в MASK). */
    int slot = -1;
    for (u32 i = 0; i < vcnt; i++) {
        u32 mlo, mhi;
        rdmsr(IA32_MTRR_PHYSBASE0 + i * 2 + 1, &mlo, &mhi);
        if (!(mlo & (1u << 11))) { slot = (int)i; break; }
    }
    if (slot < 0) return 0;

    u32 fl = irq_save();

    /* Стандартная последовательность смены MTRR: отключить кеш,
       сбросить содержимое, снять глобальное разрешение MTRR. */
    u32 cr0;
    __asm__ volatile("movl %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movl %0, %%cr0" : : "r"((cr0 | 0x40000000u) & ~0x20000000u));
    __asm__ volatile("wbinvd");

    u32 def_lo, def_hi;
    rdmsr(IA32_MTRR_DEF_TYPE, &def_lo, &def_hi);
    wrmsr(IA32_MTRR_DEF_TYPE, def_lo & ~(1u << 11), def_hi);

    /* PHYSBASE: адрес + тип; PHYSMASK: маска длины + бит valid. */
    wrmsr(IA32_MTRR_PHYSBASE0 + slot * 2,
          (base & 0xFFFFF000u) | MTRR_TYPE_WC, 0);
    wrmsr(IA32_MTRR_PHYSBASE0 + slot * 2 + 1,
          ((~(len - 1)) & 0xFFFFF000u) | (1u << 11), 0x0000000F);

    wrmsr(IA32_MTRR_DEF_TYPE, def_lo | (1u << 11), def_hi);

    __asm__ volatile("wbinvd");
    __asm__ volatile("movl %0, %%cr0" : : "r"(cr0));

    irq_restore(fl);

    mtrr_ok = 1;
    mtrr_slot_used = slot;
    return 1;
}

u32 mtrr_wc_bytes(void) {
    return mtrr_ok ? 1 : 0;
}

int mtrr_slot(void) { return mtrr_slot_used; }
