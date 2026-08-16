#include "kernel.h"

void *memset(void *d, int c, size_t n) {
    u8 *p = (u8 *)d;
    u8 v = (u8)c;

    /* Same trick: fill the aligned middle word by word. */
    if (n >= 8) {
        while (((u32)p & 3u) && n) { *p++ = v; n--; }
        u32 w = ((u32)v << 24) | ((u32)v << 16) | ((u32)v << 8) | v;
        u32 *pw = (u32 *)p;
        size_t words = n >> 2;
        for (size_t i = 0; i < words; i++) pw[i] = w;
        p += words << 2; n &= 3u;
    }
    while (n--) *p++ = v;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    u8 *dp = (u8 *)d; const u8 *sp = (const u8 *)s;

    /* Byte-by-byte copying makes four times as many memory accesses as
       necessary. When both pointers are aligned we move 4-byte words.
       This function is used everywhere, including the back buffer. */
    if ((((u32)dp | (u32)sp) & 3u) == 0) {
        u32 *dw = (u32 *)dp; const u32 *sw = (const u32 *)sp;
        size_t words = n >> 2;
        for (size_t i = 0; i < words; i++) dw[i] = sw[i];
        dp += words << 2; sp += words << 2; n &= 3u;
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    u8 *dp = (u8 *)d; const u8 *sp = (const u8 *)s;
    if (dp == sp || n == 0) return d;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    if (n) d[i] = 0;
    return d;
}

int atoi(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

int str_isnum(const char *s) {
    if (!*s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
}

void to_upper(char *s) {
    while (*s) { if (*s >= 'a' && *s <= 'z') *s -= 32; s++; }
}
