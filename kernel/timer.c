/* PIT (channel 0) + system clock + PC speaker */
#include "kernel.h"

static volatile u64 ticks = 0;
static u32 frequency = 100;

static void timer_cb(registers_t *r) {
    (void)r;
    ticks++;
    kbd_poll();          /* fallback in case IRQ1 does not work */
    schedule();          /* pre-emptive multitasking */
}

u64 timer_ticks(void)   { return ticks; }
u32 timer_hz(void)      { return frequency; }
u32 timer_seconds(void) { return (u32)ticks / frequency; }

void timer_init(u32 hz) {
    /* a zero or absurd frequency would break the division and the PIT */
    if (hz < 19) hz = 19;              /* any lower and the divisor no longer fits in 16 bits */
    if (hz > 10000) hz = 10000;
    frequency = hz;
    u32 divisor = 1193180 / hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor == 0) divisor = 1;
    outb(0x43, 0x36);
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)((divisor >> 8) & 0xFF));
    irq_install_handler(0, timer_cb);
}

void sleep_ms(u32 ms) {
    u64 target = ticks + (u64)(ms / 1000u * frequency + (ms % 1000u) * frequency / 1000u);
    while (ticks < target) __asm__ volatile("hlt");
}

void beep(u32 freq, u32 ms) {
    if (freq < 19 || freq > 20000) return;    /* outside the audible/allowed range */
    u32 div = 1193180 / freq;
    if (!div) return;
    outb(0x43, 0xB6);
    outb(0x42, (u8)(div & 0xFF));
    outb(0x42, (u8)(div >> 8));
    outb(0x61, inb(0x61) | 3);
    sleep_ms(ms);
    outb(0x61, inb(0x61) & 0xFC);
}
