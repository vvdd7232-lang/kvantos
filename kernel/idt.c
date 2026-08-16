/* Interrupt descriptor table, PIC 8259, ISR/IRQ dispatch */
#include "kernel.h"

struct idt_entry {
    u16 base_low;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_high;
} __attribute__((packed));

struct idt_ptr { u16 limit; u32 base; } __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   ip;
static isr_t isr_handlers[256];

#define D(n) extern void isr##n(void);
D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7) D(8) D(9) D(10) D(11) D(12) D(13) D(14) D(15)
D(16) D(17) D(18) D(19) D(20) D(21) D(22) D(23) D(24) D(25) D(26) D(27) D(28) D(29) D(30) D(31)
D(128)
#undef D
#define Q(n) extern void irq##n(void);
Q(0) Q(1) Q(2) Q(3) Q(4) Q(5) Q(6) Q(7) Q(8) Q(9) Q(10) Q(11) Q(12) Q(13) Q(14) Q(15)
#undef Q

static const char *exception_names[32] = {
    "Divide by zero", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
    "Stack-segment fault", "General protection fault", "Page fault", "Reserved",
    "x87 FPU error", "Alignment check", "Machine check", "SIMD FP exception",
    "Virtualization exception", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor injection",
    "VMM communication", "Security exception", "Reserved", "Reserved"
};

static void idt_set(int n, u32 base, u16 sel, u8 flags) {
    idt[n].base_low  = (u16)(base & 0xFFFF);
    idt[n].base_high = (u16)((base >> 16) & 0xFFFF);
    idt[n].sel   = sel;
    idt[n].zero  = 0;
    idt[n].flags = flags;
}

void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   /* master -> 32 */
    outb(0xA1, 0x28); io_wait();   /* slave  -> 40 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    /* Unmask everything: masking individual lines buys nothing, an
       unhandled IRQ simply gets an EOI. IRQ0 (timer) and IRQ1
       (keyboard) must stay open. */
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

void isr_install_handler(u8 n, isr_t h) { isr_handlers[n] = h; }
void irq_install_handler(u8 irq, isr_t h) { isr_handlers[32 + irq] = h; }

void isr_handler(registers_t *r) {
    isr_t h = isr_handlers[r->int_no & 0xFF];
    if (h) { h(r); return; }
    /* Exception inside an application: kill it, keep the system alive */
    if (r->int_no < 32 && kapp_in_app()) kapp_recover(exception_names[r->int_no]);
    if (r->int_no < 32) panic(exception_names[r->int_no], r);
    else kprintf(T("[!] Unhandled interrupt %u\n", "[!] Необработанное прерывание %u\n"), r->int_no);
}

void irq_handler(registers_t *r) {
    u32 irq = r->int_no - 32;
    /* Send the EOI before the handler: the handler may switch tasks */
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    isr_t h = isr_handlers[r->int_no];
    if (h) h(r);
}

void idt_init(void) {
    ip.limit = sizeof(idt) - 1;
    ip.base  = (u32)&idt;
    memset(&idt, 0, sizeof(idt));
    memset(&isr_handlers, 0, sizeof(isr_handlers));

    pic_remap();

#define S(n) idt_set(n, (u32)isr##n, 0x08, 0x8E);
    S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7) S(8) S(9) S(10) S(11) S(12) S(13) S(14) S(15)
    S(16) S(17) S(18) S(19) S(20) S(21) S(22) S(23) S(24) S(25) S(26) S(27) S(28) S(29) S(30) S(31)
#undef S
    idt_set(128, (u32)isr128, 0x08, 0xEE);   /* syscall, reachable from ring 3 */

#define I(n, v) idt_set(v, (u32)irq##n, 0x08, 0x8E);
    I(0,32) I(1,33) I(2,34) I(3,35) I(4,36) I(5,37) I(6,38) I(7,39)
    I(8,40) I(9,41) I(10,42) I(11,43) I(12,44) I(13,45) I(14,46) I(15,47)
#undef I

    idt_flush((u32)&ip);
}
