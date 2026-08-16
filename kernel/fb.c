/* ============================================================
 *  KvantOS - linear framebuffer driver (VBE through GRUB)
 *  Supports 32/24/16 bits per pixel, primitives and text drawing.
 * ============================================================ */
#include "kernel.h"

extern const u8 kv_font8x16[256 * 16];

static u8  *fb_hw    = NULL;      /* the physical adapter buffer */
static u8  *fb_draw  = NULL;      /* current drawing target */
static u32  fb_w = 0, fb_h = 0, fb_pitch = 0;
static u8   fb_bpp = 0;
static int  fb_ok = 0;

/* positions of the colour fields */
static u8 r_pos = 16, r_size = 8, g_pos = 8, g_size = 8, b_pos = 0, b_size = 8;

int  fb_active(void)  { return fb_ok; }
u32  fb_width(void)   { return fb_w; }
u32  fb_height(void)  { return fb_h; }
u32  fb_pitch_get(void) { return fb_pitch; }
u8   fb_bpp_get(void) { return fb_bpp; }
u32  fb_base(void)    { return (u32)fb_hw; }
u32  fb_bytes(void)   { return fb_pitch * fb_h; }

void fb_set_target(void *p) { fb_draw = p ? (u8 *)p : fb_hw; }
void *fb_get_hw(void)       { return fb_hw; }

/* The framebuffer usually sits outside the mapped 16 MiB (around
   0xFD000000). Called right after paging is enabled. */
int fb_map(void) {
    if (!fb_ok) return 0;
    u32 base = (u32)fb_hw, size = fb_pitch * fb_h;

    /* Let the CPU combine writes into video memory (WC). Without it
       every pixel write is a separate bus transaction and pushing a
       frame on real hardware becomes several times slower. */
    mtrr_set_wc(base, size);

    if (base + size <= 0x01000000u) return 1;      /* already mapped */
    return paging_map_range(base, size, 1) == 0;
}

int fb_init(const multiboot_info_t *mbi) {
    if (!(mbi->flags & (1u << 12))) return 0;              /* no information about the buffer */
    if (mbi->framebuffer_type != 1) return 0;              /* direct RGB is required */
    if (mbi->framebuffer_addr_high) return 0;              /* above 4 GiB is not supported */
    u8 bpp = mbi->framebuffer_bpp;
    if (bpp != 32 && bpp != 24 && bpp != 16) return 0;

    fb_hw    = (u8 *)(u32)mbi->framebuffer_addr_low;
    fb_draw  = fb_hw;
    fb_w     = mbi->framebuffer_width;
    fb_h     = mbi->framebuffer_height;
    fb_pitch = mbi->framebuffer_pitch;
    fb_bpp   = bpp;

    r_pos = mbi->fb_red_position;   r_size = mbi->fb_red_mask_size;
    g_pos = mbi->fb_green_position; g_size = mbi->fb_green_mask_size;
    b_pos = mbi->fb_blue_position;  b_size = mbi->fb_blue_mask_size;
    if (!r_size || !g_size || !b_size) {  /* safety net */
        r_pos = 16; g_pos = 8; b_pos = 0; r_size = g_size = b_size = 8;
    }
    fb_ok = 1;
    return 1;
}

/* Rebind the driver to a new mode after a resolution change.
   Colour positions for BGA/SVGA are BGRX at 32/24 bpp, RGB565 at 16. */
int fb_remap(u32 phys, u32 w, u32 h, u32 pitch, u8 bpp) {
    if (!phys || !w || !h || !pitch) return -1;
    if (bpp != 32 && bpp != 24 && bpp != 16) return -1;

    u32 size = pitch * h;
    if (phys + size > 0x01000000u)
        if (paging_map_range(phys, size, 1) < 0) return -1;

    /* The write-combining region was sized for the PREVIOUS mode. After a
       switch to a larger resolution the tail of the buffer stays
       uncacheable, and every frame past that boundary crawls - on real
       hardware a 1920x1080 frame then takes hundreds of milliseconds and
       the desktop looks hung. QEMU keeps the framebuffer in ordinary host
       RAM, so it never shows this. Re-arm WC for the new geometry. */
    mtrr_set_wc(phys, size);

    fb_hw    = (u8 *)phys;
    fb_draw  = fb_hw;
    fb_w     = w;
    fb_h     = h;
    fb_pitch = pitch;
    fb_bpp   = bpp;

    if (bpp == 16) { r_pos = 11; r_size = 5; g_pos = 5; g_size = 6; b_pos = 0; b_size = 5; }
    else           { r_pos = 16; g_pos = 8; b_pos = 0; r_size = g_size = b_size = 8; }

    fb_ok = 1;
    return 0;
}

/* Pack a colour for the current mode format */
u32 fb_rgb(u8 r, u8 g, u8 b) {
    u32 rv = (u32)r >> (8 - r_size);
    u32 gv = (u32)g >> (8 - g_size);
    u32 bv = (u32)b >> (8 - b_size);
    return (rv << r_pos) | (gv << g_pos) | (bv << b_pos);
}

static inline void put_raw(u32 x, u32 y, u32 c) {
    u8 *p = fb_draw + y * fb_pitch + x * (fb_bpp >> 3);
    switch (fb_bpp) {
        case 32: *(u32 *)p = c; break;
        case 24: p[0] = (u8)(c); p[1] = (u8)(c >> 8); p[2] = (u8)(c >> 16); break;
        case 16: *(u16 *)p = (u16)c; break;
    }
}

void fb_pixel(u32 x, u32 y, u32 c) {
    if (!fb_ok || x >= fb_w || y >= fb_h) return;
    put_raw(x, y, c);
}

void fb_fill(i32 x, i32 y, i32 w, i32 h, u32 c) {
    if (!fb_ok) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (i32)fb_w) w = (i32)fb_w - x;
    if (y + h > (i32)fb_h) h = (i32)fb_h - y;
    if (w <= 0 || h <= 0) return;

    for (i32 j = 0; j < h; j++) {
        u8 *row = fb_draw + (u32)(y + j) * fb_pitch + (u32)x * (fb_bpp >> 3);
        if (fb_bpp == 32) {
            u32 *p = (u32 *)row;
            for (i32 i = 0; i < w; i++) p[i] = c;
        } else {
            for (i32 i = 0; i < w; i++) put_raw((u32)(x + i), (u32)(y + j), c);
        }
    }
}

void fb_rect(i32 x, i32 y, i32 w, i32 h, u32 c) {
    fb_fill(x, y, w, 1, c);
    fb_fill(x, y + h - 1, w, 1, c);
    fb_fill(x, y, 1, h, c);
    fb_fill(x + w - 1, y, 1, h, c);
}

/* Vertical gradient */
void fb_gradient_v(i32 x, i32 y, i32 w, i32 h,
                   u8 r1, u8 g1, u8 b1, u8 r2, u8 g2, u8 b2) {
    if (h <= 0) return;
    for (i32 j = 0; j < h; j++) {
        u32 t = (u32)j * 256u / (u32)h;
        u8 r = (u8)(r1 + ((i32)(r2 - r1) * (i32)t) / 256);
        u8 g = (u8)(g1 + ((i32)(g2 - g1) * (i32)t) / 256);
        u8 b = (u8)(b1 + ((i32)(b2 - b1) * (i32)t) / 256);
        fb_fill(x, y + j, w, 1, fb_rgb(r, g, b));
    }
}

/* Rounded rectangle (radius 4) */
void fb_round_fill(i32 x, i32 y, i32 w, i32 h, u32 c) {
    const i32 r = 4;
    if (w < 2 * r || h < 2 * r) { fb_fill(x, y, w, h, c); return; }
    fb_fill(x + r, y, w - 2 * r, h, c);
    fb_fill(x, y + r, r, h - 2 * r, c);
    fb_fill(x + w - r, y + r, r, h - 2 * r, c);
    static const i32 off[4] = { 2, 1, 1, 0 };   /* corner profile */
    for (i32 i = 0; i < r; i++) {
        i32 o = off[i];
        fb_fill(x + o, y + r - 1 - i, r - o, 1, c);
        fb_fill(x + w - r, y + r - 1 - i, r - o, 1, c);
        fb_fill(x + o, y + h - r + i, r - o, 1, c);
        fb_fill(x + w - r, y + h - r + i, r - o, 1, c);
    }
}

/* ---------- text ---------- */

/* A single CP866 character. bg = 0xFFFFFFFF means a transparent background */
void fb_glyph(i32 x, i32 y, u8 ch, u32 fg, u32 bg) {
    if (!fb_ok) return;
    const u8 *g = &kv_font8x16[(u32)ch * 16];

    /* Fast path: the glyph fits entirely on screen and the mode is
       32-bit. Neither per-pixel clipping nor address recomputation is
       needed here - we walk the row with a pointer. There is a lot of
       text on screen, so this loop noticeably affects overall speed. */
    if (fb_bpp == 32 && x >= 0 && y >= 0 &&
        x + 8 <= (i32)fb_w && y + 16 <= (i32)fb_h) {
        u8 *row = fb_draw + (u32)y * fb_pitch + (u32)x * 4;
        if (bg == 0xFFFFFFFFu) {
            for (i32 r = 0; r < 16; r++, row += fb_pitch) {
                u8 bits = g[r];
                if (!bits) continue;                 /* an empty glyph row */
                u32 *p = (u32 *)row;
                if (bits & 0x80) p[0] = fg;
                if (bits & 0x40) p[1] = fg;
                if (bits & 0x20) p[2] = fg;
                if (bits & 0x10) p[3] = fg;
                if (bits & 0x08) p[4] = fg;
                if (bits & 0x04) p[5] = fg;
                if (bits & 0x02) p[6] = fg;
                if (bits & 0x01) p[7] = fg;
            }
        } else {
            for (i32 r = 0; r < 16; r++, row += fb_pitch) {
                u8 bits = g[r];
                u32 *p = (u32 *)row;
                p[0] = (bits & 0x80) ? fg : bg;
                p[1] = (bits & 0x40) ? fg : bg;
                p[2] = (bits & 0x20) ? fg : bg;
                p[3] = (bits & 0x10) ? fg : bg;
                p[4] = (bits & 0x08) ? fg : bg;
                p[5] = (bits & 0x04) ? fg : bg;
                p[6] = (bits & 0x02) ? fg : bg;
                p[7] = (bits & 0x01) ? fg : bg;
            }
        }
        return;
    }

    /* General path: edge clipping, 16/24 bpp. */
    for (i32 row = 0; row < 16; row++) {
        u8 bits = g[row];
        i32 py = y + row;
        if (py < 0 || py >= (i32)fb_h) continue;
        for (i32 col = 0; col < 8; col++) {
            i32 px = x + col;
            if (px < 0 || px >= (i32)fb_w) continue;
            if (bits & (0x80 >> col)) put_raw((u32)px, (u32)py, fg);
            else if (bg != 0xFFFFFFFFu) put_raw((u32)px, (u32)py, bg);
        }
    }
}

/* A UTF-8 string */
void fb_text(i32 x, i32 y, const char *s, u32 fg, u32 bg) {
    if (!fb_ok) return;
    while (*s) {
        u32 cp = utf8_next(&s);
        fb_glyph(x, y, cp866_from_unicode(cp), fg, bg);
        x += 8;
    }
}

/* A string centred inside an area of width w */
void fb_text_center(i32 x, i32 y, i32 w, const char *s, u32 fg, u32 bg) {
    i32 tw = (i32)utf8_len(s) * 8;
    fb_text(x + (w - tw) / 2, y, s, fg, bg);
}

void fb_clear(u32 c) { fb_fill(0, 0, (i32)fb_w, (i32)fb_h, c); }

/* Fast blit of the back buffer to the screen */
/* Blit only a part of the frame (the band of rows y0..y1).
   The GUI redraws the whole back buffer, yet only a small part of the
   screen actually changes. Pushing all 3 MiB across the bus every frame
   is pointless - only the affected rows are sent. */
void fb_present_rows(const void *back, u32 y0, u32 y1) {
    if (!fb_ok || !back) return;
    if (y1 > fb_h) y1 = fb_h;
    if (y0 >= y1) return;

    const u8 *sb = (const u8 *)back + (u32)y0 * fb_pitch;
    u8       *db = (u8 *)fb_hw     + (u32)y0 * fb_pitch;
    u32 words = ((y1 - y0) * fb_pitch) >> 2;

    const u32 *s = (const u32 *)sb;
    u32       *d = (u32 *)db;
    u32 i = 0;
    for (; i + 8 <= words; i += 8) {
        d[i]     = s[i];     d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2]; d[i + 3] = s[i + 3];
        d[i + 4] = s[i + 4]; d[i + 5] = s[i + 5];
        d[i + 6] = s[i + 6]; d[i + 7] = s[i + 7];
    }
    for (; i < words; i++) d[i] = s[i];
}

void fb_present(const void *back) {
    if (!fb_ok || !back) return;
    u32 n = fb_pitch * fb_h;
    const u32 *s = (const u32 *)back;
    u32 *d = (u32 *)fb_hw;
    u32 words = n >> 2;

    /* The loop is unrolled to 8 words (32 bytes): fewer condition checks
       and better filling of the write-combining buffers. At 1024x768x32
       that is 3 MiB per frame - the hottest loop in the whole system. */
    u32 i = 0;
    for (; i + 8 <= words; i += 8) {
        d[i]     = s[i];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2];
        d[i + 3] = s[i + 3];
        d[i + 4] = s[i + 4];
        d[i + 5] = s[i + 5];
        d[i + 6] = s[i + 6];
        d[i + 7] = s[i + 7];
    }
    for (; i < words; i++) d[i] = s[i];
}

/* Scroll a screen area up by dy pixels (for the text console) */
void fb_scroll_up(u32 top, u32 bottom, u32 dy, u32 bg) {
    if (!fb_ok || bottom <= top + dy) return;
    u32 rows = bottom - top - dy;
    u8 *dst = fb_hw + (u32)top * fb_pitch;
    u8 *src = dst + (u32)dy * fb_pitch;
    u32 bytes = rows * fb_pitch;
    u32 words = bytes >> 2;
    u32 *d32 = (u32 *)dst, *s32 = (u32 *)src;
    for (u32 i = 0; i < words; i++) d32[i] = s32[i];
    u8 *save = fb_draw;
    fb_draw = fb_hw;
    fb_fill(0, (i32)(bottom - dy), (i32)fb_w, (i32)dy, bg);
    fb_draw = save;
}
