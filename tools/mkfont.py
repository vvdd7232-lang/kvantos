#!/usr/bin/env python3
"""Собирает 8x16 VGA-шрифт в раскладке CP866 из системных PSF-шрифтов.
Результат: kernel/font8x16.c (массив 256*16 байт)."""
import gzip, struct, sys, os

PSF1_MAGIC = b'\x36\x04'
PSF2_MAGIC = b'\x72\xb5\x4a\x86'


def load_psf(path):
    data = gzip.open(path, 'rb').read() if path.endswith('.gz') else open(path, 'rb').read()
    glyphs, uni = [], {}
    if data[:2] == PSF1_MAGIC:
        mode, charsize = data[2], data[3]
        count = 512 if (mode & 0x01) else 256
        off = 4
        for i in range(count):
            glyphs.append(data[off + i * charsize: off + (i + 1) * charsize])
        off += count * charsize
        if mode & 0x02 or mode & 0x04:
            i, pos = 0, off
            while pos < len(data) and i < count:
                seq = b''
                while pos + 1 < len(data):
                    w = data[pos] | (data[pos + 1] << 8)
                    pos += 2
                    if w == 0xFFFF:
                        break
                    if w == 0xFFFE:
                        continue
                    seq += struct.pack('<H', w)
                for j in range(0, len(seq), 2):
                    cp = struct.unpack('<H', seq[j:j + 2])[0]
                    uni.setdefault(cp, i)
                i += 1
        width, height = 8, charsize
    elif data[:4] == PSF2_MAGIC:
        ver, hdr, flags, length, charsize, height, width = struct.unpack('<IIIIIII', data[4:32])
        off = hdr
        for i in range(length):
            glyphs.append(data[off + i * charsize: off + (i + 1) * charsize])
        off += length * charsize
        if flags & 0x01:
            i, pos = 0, off
            while pos < len(data) and i < length:
                start = pos
                while pos < len(data) and data[pos] not in (0xFF, 0xFE):
                    pos += 1
                try:
                    for ch in data[start:pos].decode('utf-8', 'ignore'):
                        uni.setdefault(ord(ch), i)
                except Exception:
                    pass
                while pos < len(data) and data[pos] == 0xFE:
                    pos += 1
                    s2 = pos
                    while pos < len(data) and data[pos] not in (0xFF, 0xFE):
                        pos += 1
                if pos < len(data) and data[pos] == 0xFF:
                    pos += 1
                i += 1
    else:
        raise ValueError('не PSF: ' + path)
    return {'glyphs': glyphs, 'uni': uni, 'w': width, 'h': height}


def glyph_for(font, cp, target_h=16):
    idx = font['uni'].get(cp)
    if idx is None or idx >= len(font['glyphs']):
        return None
    g = font['glyphs'][idx]
    h = font['h']
    bytes_per_row = max(1, (font['w'] + 7) // 8)
    rows = [g[r * bytes_per_row] if r * bytes_per_row < len(g) else 0 for r in range(h)]
    if h == target_h:
        return rows
    if h < target_h:                       # вертикальное центрирование
        pad = target_h - h
        top = pad // 2
        return [0] * top + rows + [0] * (pad - top)
    start = (h - target_h) // 2
    return rows[start:start + target_h]


# --- CP866: 0x00-0x7F ASCII, 0x80-0xAF А-я(часть), 0xB0-0xDF псевдографика,
# --- 0xE0-0xEF р-я, 0xF0.. Ё ё и прочее
CP866 = {}
for i in range(0x20, 0x7F):
    CP866[i] = i
# А..Я -> 0x80..0x9F ; а..п -> 0xA0..0xAF
for k in range(0x20):
    CP866[0x80 + k] = 0x410 + k
for k in range(0x10):
    CP866[0xA0 + k] = 0x430 + k
# р..я -> 0xE0..0xEF
for k in range(0x10):
    CP866[0xE0 + k] = 0x440 + k
CP866[0xF0] = 0x401   # Ё
CP866[0xF1] = 0x451   # ё
CP866[0xFC] = 0x2116  # №
CP866[0xF8] = 0xB0    # °
CP866[0xFA] = 0xB7    # ·
CP866[0xFD] = 0xA4    # ¤
# псевдографика 0xB0..0xDF
BOX = {
    0xB0: 0x2591, 0xB1: 0x2592, 0xB2: 0x2593, 0xB3: 0x2502, 0xB4: 0x2524,
    0xB5: 0x2561, 0xB6: 0x2562, 0xB7: 0x2556, 0xB8: 0x2555, 0xB9: 0x2563,
    0xBA: 0x2551, 0xBB: 0x2557, 0xBC: 0x255D, 0xBD: 0x255C, 0xBE: 0x255B,
    0xBF: 0x2510, 0xC0: 0x2514, 0xC1: 0x2534, 0xC2: 0x252C, 0xC3: 0x251C,
    0xC4: 0x2500, 0xC5: 0x253C, 0xC6: 0x255E, 0xC7: 0x255F, 0xC8: 0x255A,
    0xC9: 0x2554, 0xCA: 0x2569, 0xCB: 0x2566, 0xCC: 0x2560, 0xCD: 0x2550,
    0xCE: 0x256C, 0xCF: 0x2567, 0xD0: 0x2568, 0xD1: 0x2564, 0xD2: 0x2565,
    0xD3: 0x2559, 0xD4: 0x2558, 0xD5: 0x2552, 0xD6: 0x2553, 0xD7: 0x256B,
    0xD8: 0x256A, 0xD9: 0x2518, 0xDA: 0x250C, 0xDB: 0x2588, 0xDC: 0x2584,
    0xDD: 0x258C, 0xDE: 0x2590, 0xDF: 0x2580,
}
CP866.update(BOX)
# стрелки и прочее в служебных ячейках управляющей зоны (для UI)
CP866[0x01] = 0x263A
CP866[0x03] = 0x2665
CP866[0x04] = 0x2666
CP866[0x0F] = 0x263C
CP866[0x10] = 0x25BA
CP866[0x11] = 0x25C4
CP866[0x18] = 0x2191
CP866[0x19] = 0x2193
CP866[0x1A] = 0x2192
CP866[0x1B] = 0x2190
CP866[0x07] = 0x2022

SRC = [
    '/usr/share/consolefonts/CyrSlav-Fixed16.psf.gz',
    '/usr/share/consolefonts/Lat2-VGA16.psf.gz',
    '/usr/share/consolefonts/Lat2-Fixed16.psf.gz',
    '/usr/share/consolefonts/Uni2-VGA16.psf.gz',
    '/usr/share/consolefonts/Uni3-Fixed16.psf.gz',
    '/usr/share/consolefonts/CyrAsia-Fixed16.psf.gz',
]

fonts = []
for p in SRC:
    if os.path.exists(p):
        try:
            fonts.append(load_psf(p))
        except Exception as e:
            print('пропуск', p, e, file=sys.stderr)
if not fonts:
    sys.exit('нет исходных шрифтов')

# Глифы, которые дорисовываем вручную (в исходных шрифтах их нет)
MANUAL = {
    0xB0: [0x00, 0x44, 0x00, 0x11, 0x00, 0x44, 0x00, 0x11] * 2,          # ░
    0xB1: [0xAA, 0x55] * 8,                                              # ▒
    0xB2: [0xBB, 0xEE] * 8,                                              # ▓
    0xDB: [0xFF] * 16,                                                   # █
    0xDC: [0x00] * 8 + [0xFF] * 8,                                       # ▄
    0xDD: [0xF0] * 16,                                                   # ▌
    0xDE: [0x0F] * 16,                                                   # ▐
    0xDF: [0xFF] * 8 + [0x00] * 8,                                       # ▀
    0x07: [0, 0, 0, 0, 0, 0x18, 0x3C, 0x3C, 0x18, 0, 0, 0, 0, 0, 0, 0],  # •
    0x10: [0, 0, 0, 0x20, 0x30, 0x38, 0x3C, 0x3E, 0x3C, 0x38, 0x30, 0x20, 0, 0, 0, 0],
    0x11: [0, 0, 0, 0x04, 0x0C, 0x1C, 0x3C, 0x7C, 0x3C, 0x1C, 0x0C, 0x04, 0, 0, 0, 0],
    0x18: [0, 0, 0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0, 0, 0, 0],
    0x19: [0, 0, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x3C, 0x18, 0, 0, 0, 0],
    0x1A: [0, 0, 0, 0, 0x08, 0x0C, 0x7E, 0x7F, 0x7E, 0x0C, 0x08, 0, 0, 0, 0, 0],
    0x1B: [0, 0, 0, 0, 0x10, 0x30, 0x7E, 0xFE, 0x7E, 0x30, 0x10, 0, 0, 0, 0, 0],
    0x01: [0, 0, 0x3C, 0x42, 0xA5, 0x81, 0x81, 0xA5, 0x99, 0x42, 0x3C, 0, 0, 0, 0, 0],
    0x03: [0, 0, 0, 0x6C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38, 0x10, 0, 0, 0, 0, 0, 0],
    0x0F: [0, 0, 0x18, 0x5A, 0x3C, 0x66, 0xE7, 0x66, 0x3C, 0x5A, 0x18, 0, 0, 0, 0, 0],
}

table = []
missing = []
for code in range(256):
    cp = CP866.get(code)
    rows = None
    if code in MANUAL:
        rows = MANUAL[code][:16]
    elif cp is not None:
        for f in fonts:
            rows = glyph_for(f, cp)
            if rows and any(rows):
                break
            rows = None
    if rows is None:
        rows = [0] * 16
        if cp:
            missing.append((hex(code), hex(cp)))
    table.append(rows)

with open(os.path.join(os.path.dirname(__file__), '..', 'kernel', 'font8x16.c'), 'w') as f:
    f.write('/* Сгенерировано tools/mkfont.py - VGA 8x16, раскладка CP866 */\n')
    f.write('#include "kernel.h"\n\n')
    f.write('const u8 kv_font8x16[256 * 16] = {\n')
    for code, rows in enumerate(table):
        f.write('    ' + ' '.join('0x%02X,' % r for r in rows) + '  /* 0x%02X */\n' % code)
    f.write('};\n')

print('готово; пустых глифов:', len(missing))
if missing[:10]:
    print('нет:', missing[:10])
