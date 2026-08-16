/* ============================================================
 *  KvantOS - перекодировка UTF-8 -> CP866 (раскладка знакогенератора)
 * ============================================================ */
#include "kernel.h"

u8 cp866_from_unicode(u32 cp) {
    if (cp < 0x80) return (u8)cp;
    if (cp >= 0x410 && cp <= 0x42F) return (u8)(0x80 + (cp - 0x410));   /* А-Я */
    if (cp >= 0x430 && cp <= 0x43F) return (u8)(0xA0 + (cp - 0x430));   /* а-п */
    if (cp >= 0x440 && cp <= 0x44F) return (u8)(0xE0 + (cp - 0x440));   /* р-я */
    if (cp == 0x401) return 0xF0;   /* Ё */
    if (cp == 0x451) return 0xF1;   /* ё */
    if (cp == 0x2116) return 0xFC;  /* № */
    if (cp == 0xB0) return 0xF8;    /* ° */
    if (cp == 0xB7) return 0xFA;    /* · */
    switch (cp) {
        case 0x2591: return 0xB0; case 0x2592: return 0xB1; case 0x2593: return 0xB2;
        case 0x2502: return 0xB3; case 0x2524: return 0xB4; case 0x2563: return 0xB9;
        case 0x2551: return 0xBA; case 0x2557: return 0xBB; case 0x255D: return 0xBC;
        case 0x2510: return 0xBF; case 0x2514: return 0xC0; case 0x2534: return 0xC1;
        case 0x252C: return 0xC2; case 0x251C: return 0xC3; case 0x2500: return 0xC4;
        case 0x253C: return 0xC5; case 0x255A: return 0xC8; case 0x2554: return 0xC9;
        case 0x2569: return 0xCA; case 0x2566: return 0xCB; case 0x2560: return 0xCC;
        case 0x2550: return 0xCD; case 0x256C: return 0xCE; case 0x2518: return 0xD9;
        case 0x250C: return 0xDA; case 0x2588: return 0xDB; case 0x2584: return 0xDC;
        case 0x258C: return 0xDD; case 0x2590: return 0xDE; case 0x2580: return 0xDF;
        case 0x2022: return 0x07; case 0x263A: return 0x01; case 0x2665: return 0x03;
        case 0x2666: return 0x04; case 0x263C: return 0x0F; case 0x25BA: return 0x10;
        case 0x25C4: return 0x11; case 0x2191: return 0x18; case 0x2193: return 0x19;
        case 0x2192: return 0x1A; case 0x2190: return 0x1B;
        case 0x00AB: return '<';  case 0x00BB: return '>';
        case 0x2014: case 0x2013: return 0xC4;
        case 0x201C: case 0x201D: return '"';
    }
    return '?';
}

/* Прочитать один кодпойнт UTF-8 и сдвинуть указатель */
u32 utf8_next(const char **ps) {
    const char *s = *ps;
    u8 c = (u8)*s++;
    u32 cp = c;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    while (extra-- > 0 && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
    *ps = s;
    return cp;
}

/* Длина UTF-8 строки в символах */
u32 utf8_len(const char *s) {
    u32 n = 0;
    while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; }
    return n;
}

/* Перекодировать строку целиком; возвращает число символов */
u32 utf8_to_cp866(const char *s, u8 *out, u32 max) {
    u32 n = 0;
    while (*s && n < max) out[n++] = cp866_from_unicode(utf8_next(&s));
    return n;
}
