/* Мини-printf ядра: %d %i %u %x %X %p %s %c %% + ширина с нулями (%08x) */
#include "kernel.h"

void kputc(char c) { vga_putc(c); serial_putc(c); }
void kputs(const char *s) { while (*s) kputc(*s++); }

static void emit_screen(char c, void *ctx) { (void)ctx; kputc(c); }

typedef struct { char *buf; size_t size; size_t pos; } sbuf_t;
static void emit_buf(char c, void *ctx) {
    sbuf_t *s = (sbuf_t *)ctx;
    if (s->pos + 1 < s->size) s->buf[s->pos] = c;
    s->pos++;
}

static void out_str(void (*emit)(char, void *), void *ctx, const char *s) {
    while (*s) emit(*s++, ctx);
}

static void out_num(void (*emit)(char, void *), void *ctx,
                    u32 value, u32 base, int is_signed, int width, int zero, int upper) {
    char tmp[36];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0, neg = 0;
    u32 v = value;

    if (is_signed && (i32)value < 0) { neg = 1; v = (u32)(-(i32)value); }
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v % base]; v /= base; }
    if (neg) tmp[i++] = '-';
    while (i < width) tmp[i++] = zero ? '0' : ' ';
    while (i--) emit(tmp[i], ctx);
}

void kvprintf(void (*emit)(char, void *), void *ctx, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(*fmt, ctx); continue; }
        fmt++;
        int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt) {
            case 'd': case 'i': out_num(emit, ctx, va_arg(ap, u32), 10, 1, width, zero, 0); break;
            case 'u':           out_num(emit, ctx, va_arg(ap, u32), 10, 0, width, zero, 0); break;
            case 'x':           out_num(emit, ctx, va_arg(ap, u32), 16, 0, width, zero, 0); break;
            case 'X':           out_num(emit, ctx, va_arg(ap, u32), 16, 0, width, zero, 1); break;
            case 'b':           out_num(emit, ctx, va_arg(ap, u32),  2, 0, width, zero, 0); break;
            case 'p':           out_str(emit, ctx, "0x");
                                out_num(emit, ctx, (u32)va_arg(ap, void *), 16, 0, 8, 1, 0); break;
            case 'c':           emit((char)va_arg(ap, int), ctx); break;
            case 's': { const char *s = va_arg(ap, const char *);
                        out_str(emit, ctx, s ? s : "(null)"); break; }
            case '%':           emit('%', ctx); break;
            default:            emit('%', ctx); emit(*fmt, ctx); break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    kvprintf(emit_screen, NULL, fmt, ap);
    va_end(ap);
}

void ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    sbuf_t s = { buf, size, 0 };
    va_list ap; va_start(ap, fmt);
    kvprintf(emit_buf, &s, fmt, ap);
    va_end(ap);
    if (size) buf[s.pos < size ? s.pos : size - 1] = 0;
}

/* Вариант с уже собранным списком аргументов: нужен таблице
   системных вызовов, где format() сама разбирает свои «...». */
void kvsnprintf_v(char *buf, size_t size, const char *fmt, va_list ap) {
    sbuf_t s = { buf, size, 0 };
    kvprintf(emit_buf, &s, fmt, ap);
    if (size) buf[s.pos < size ? s.pos : size - 1] = 0;
}
