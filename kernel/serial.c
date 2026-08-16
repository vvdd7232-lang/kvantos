/* Отладочный вывод в COM1 (0x3F8) */
#include "kernel.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);   /* 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int tx_ready(void) { return inb(COM1 + 5) & 0x20; }

void serial_putc(char c) {
    int guard = 100000;
    while (!tx_ready() && guard--) {}
    outb(COM1, (u8)c);
    if (c == '\n') serial_putc('\r');
}

void serial_puts(const char *s) { while (*s) serial_putc(*s++); }
