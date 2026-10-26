/* ============================================================
 *  KvantOS - linear framebuffer driver (VBE through GRUB)
 *  Supports 32/24/16 bits per pixel, primitives and text drawing.
 * ============================================================ */
#include "kernel.h"

extern const u8 kv_font8x16[256 * 16];
extern const u8 kv_font_aa[256 * 16 * 4];   /* 4-bit coverage, nibble-packed */

static u8  *fb_hw    = NULL;      /* the physical adapter buffer */
static u8  *fb_draw  = NULL;      /* current drawing target */
static u32  fb_w = 0, fb_h = 0, fb_pitch = 0;
static u8   fb_bpp = 0;
static int  fb_ok = 0;

#define FB_CLIP_MAX 24
static fb_clip_rect_t fb_clip_rects[FB_CLIP_MAX];
static u32 fb_clip_count = 0;
static int fb_clip_enabled = 0;

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
void *fb_get_target(void)   { return fb_draw; }

void fb_clear_clip(void) {
    fb_clip_enabled = 0;
    fb_clip_count = 0;
}

void fb_set_clip_rects(const fb_clip_rect_t *rects, u32 count) {
    fb_clip_enabled = 1;
    fb_clip_count = 0;
    if (!rects) return;

    if (count > FB_CLIP_MAX) count = FB_CLIP_MAX;
    for (u32 i = 0; i < count; i++) {
        i32 x = rects[i].x, y = rects[i].y;
        i32 w = rects[i].w, h = rects[i].h;
        if (w <= 0 || h <= 0) continue;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x >= (i32)fb_w || y >= (i32)fb_h || w <= 0 || h <= 0) continue;
        if (w > (i32)fb_w - x) w = (i32)fb_w - x;
        if (h > (i32)fb_h - y) h = (i32)fb_h - y;
        if (w <= 0 || h <= 0) continue;
        fb_clip_rects[fb_clip_count++] = (fb_clip_rect_t){ x, y, w, h };
    }
}

static int clip_has_pixel(i32 x, i32 y) {
    if (!fb_clip_enabled) return 1;
    for (u32 i = 0; i < fb_clip_count; i++) {
        const fb_clip_rect_t *r = &fb_clip_rects[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return 1;
    }
    return 0;
}

static int clip_contains_box(i32 x, i32 y, i32 w, i32 h) {
    if (!fb_clip_enabled) return 1;
    for (u32 i = 0; i < fb_clip_count; i++) {
        const fb_clip_rect_t *r = &fb_clip_rects[i];
        if (x >= r->x && y >= r->y &&
            x + w <= r->x + r->w && y + h <= r->y + r->h)
            return 1;
    }
    return 0;
}

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
    fb_clear_clip();

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
    if (!clip_has_pixel((i32)x, (i32)y)) return;
    put_raw(x, y, c);
}

/* ---------- anti-aliasing core ---------- */

static int fb_aa = 1;
void fb_set_aa(int on) { fb_aa = !!on; }
int  fb_aa_on(void)    { return fb_aa; }

static u32 get_raw(u32 x, u32 y) {
    const u8 *p = fb_draw + y * fb_pitch + x * (fb_bpp >> 3);
    switch (fb_bpp) {
        case 32: return *(const u32 *)p;
        case 24: return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
        default: return *(const u16 *)p;
    }
}

static void unpack_rgb(u32 c, int *r, int *g, int *b) {
    int rm = (1 << r_size) - 1, gm = (1 << g_size) - 1, bm = (1 << b_size) - 1;
    *r = (int)(((c >> r_pos) & (u32)rm) * 255u / (u32)rm);
    *g = (int)(((c >> g_pos) & (u32)gm) * 255u / (u32)gm);
    *b = (int)(((c >> b_pos) & (u32)bm) * 255u / (u32)bm);
}

/* fg over bg with coverage alpha 0..255, all in packed fb colours */
static u32 blend_rgb(u32 fg, u32 bg, u32 alpha) {
    if (alpha == 0) return bg;
    if (alpha >= 255) return fg;
    int fr, fg_, fb_, br, bg_, bb;
    unpack_rgb(fg, &fr, &fg_, &fb_);
    unpack_rgb(bg, &br, &bg_, &bb);
    u32 ia = 255 - alpha;
    return fb_rgb((u8)((fr * (int)alpha + br * (int)ia) / 255),
                  (u8)((fg_ * (int)alpha + bg_ * (int)ia) / 255),
                  (u8)((fb_ * (int)alpha + bb * (int)ia) / 255));
}

void fb_blend_pixel(i32 x, i32 y, u32 c, u32 alpha) {
    if (!fb_ok || x < 0 || y < 0 || x >= (i32)fb_w || y >= (i32)fb_h) return;
    if (!alpha) return;
    if (!clip_has_pixel(x, y)) return;
    if (alpha >= 255) { put_raw((u32)x, (u32)y, c); return; }
    put_raw((u32)x, (u32)y, blend_rgb(c, get_raw((u32)x, (u32)y), alpha));
}

/* Xiaolin Wu anti-aliased line */
void fb_line_aa(i32 x0, i32 y0, i32 x1, i32 y1, u32 c) {
    if (!fb_ok) return;
    int steep = 0;
    i32 dx = x1 - x0, dy = y1 - y0;
    if (dy < 0 ? -dy > (dx < 0 ? -dx : dx) : dy > (dx < 0 ? -dx : dx)) steep = 1;
    if (steep) {
        i32 t;
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        i32 t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    dx = x1 - x0; dy = y1 - y0;
    if (!dx) { fb_blend_pixel(x0, y0, c, 255); return; }
    /* y in 16.16 fixed point, gradient step per column */
    i32 grad = dy * 65536 / dx;
    i32 y = y0 * 65536; /* integer endpoints must land on their own pixels */
    for (i32 x = x0; x <= x1; x++, y += grad) {
        i32 yi = y >> 16;
        u32 frac = ((u32)y >> 8) & 0xFF;
        if (steep) {
            fb_blend_pixel(yi, x, c, 255 - frac);
            fb_blend_pixel(yi + 1, x, c, frac);
        } else {
            fb_blend_pixel(x, yi, c, 255 - frac);
            fb_blend_pixel(x, yi + 1, x, frac);
        }
    }
}

/* Filled circle with a supersampled (2x2) anti-aliased rim */
void fb_disc_aa(i32 cx, i32 cy, i32 r, u32 c) {
    if (!fb_ok || r <= 0) return;
    i32 r2 = r * r * 4;               /* radius squared in half-pixel units */
    for (i32 y = cy - r; y <= cy + r; y++) {
        for (i32 x = cx - r; x <= cx + r; x++) {
            int inside = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    i32 px = (x - cx) * 2 + sx, py = (y - cy) * 2 + sy;
                    if (px * px + py * py <= r2) inside++;
                }
            }
            if (inside) fb_blend_pixel(x, y, c, (u32)inside * 255 / 4);
        }
    }
}

/* Coverage of one pixel of a rounded-rect corner (2x2 supersampling) */
static u32 corner_cov(i32 px, i32 py, i32 ccx, i32 ccy, i32 r) {
    int inside = 0;
    i32 r2 = r * r * 4;
    for (int sy = 0; sy < 2; sy++) {
        for (int sx = 0; sx < 2; sx++) {
            i32 qx = (px - ccx) * 2 + sx, qy = (py - ccy) * 2 + sy;
            if (qx * qx + qy * qy <= r2) inside++;
        }
    }
    return (u32)inside * 255 / 4;
}

void fb_round_fill_aa(i32 x, i32 y, i32 w, i32 h, i32 r, u32 c) {
    if (!fb_ok || w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    if (r == 0 || !fb_aa) { fb_fill(x, y, w, h, c); return; }
    fb_fill(x + r, y, w - 2 * r, h, c);
    fb_fill(x, y + r, r, h - 2 * r, c);
    fb_fill(x + w - r, y + r, r, h - 2 * r, c);
    for (i32 j = 0; j < r; j++) {
        for (i32 i = 0; i < r; i++) {
            /* Mirror ONE coverage mask. Sampling the opposite corners in
               screen coordinates shifts their half-pixel samples outward. */
            u32 cov = corner_cov(i, j, r, r, r);
            if (!cov) continue;
            fb_blend_pixel(x + i, y + j, c, cov);
            fb_blend_pixel(x + w - 1 - i, y + j, c, cov);
            fb_blend_pixel(x + i, y + h - 1 - j, c, cov);
            fb_blend_pixel(x + w - 1 - i, y + h - 1 - j, c, cov);
        }
    }
}

/* Soft drop shadow: layered rings with growing blur and alpha.
   The 32-bit path blends in place with plain channel arithmetic -
   this runs for every window in every frame, so it has to be cheap. */
void fb_shadow(i32 x, i32 y, i32 w, i32 h, i32 r) {
    if (!fb_ok) return;
    u32 black = fb_rgb(0, 0, 0);
    if (!fb_aa) { fb_fill(x + 4, y + 4, w, h, black); return; }
    (void)r;
    if (fb_bpp == 32 && r_size == 8 && g_size == 8 && b_size == 8) {
        for (i32 i = 5; i >= 1; i--) {
            i32 ex = x - i, ey = y - i + 3, ew = w + 2 * i, eh = h + 2 * i;
            u32 ia = 255u - (u32)(30 - i * 3);
            if (ex < 0) { ew += ex; ex = 0; }
            if (ey < 0) { eh += ey; ey = 0; }
            if (ex + ew > (i32)fb_w) ew = (i32)fb_w - ex;
            if (ey + eh > (i32)fb_h) eh = (i32)fb_h - ey;
            if (ew <= 0 || eh <= 0) continue;
            for (i32 yy = ey; yy < ey + eh; yy++) {
                u32 *p = (u32 *)(fb_draw + (u32)yy * fb_pitch + (u32)ex * 4);
                for (i32 xx = ex; xx < ex + ew; xx++, p++) {
                    if (xx >= x && xx < x + w && yy >= y && yy < y + h) continue;
                    if (!clip_has_pixel(xx, yy)) continue;
                    u32 bgc = *p;
                    *p = (((bgc >> 16) & 255) * ia / 255 << 16) |
                         (((bgc >> 8) & 255) * ia / 255 << 8) |
                         ((bgc & 255) * ia / 255);
                }
            }
        }
        return;
    }
    for (i32 i = 5; i >= 1; i--) {
        i32 ex = x - i, ey = y - i + 3, ew = w + 2 * i, eh = h + 2 * i;
        u32 alpha = (u32)(30 - i * 3);
        for (i32 yy = ey; yy < ey + eh; yy++)
            for (i32 xx = ex; xx < ex + ew; xx++) {
                if (xx >= x && xx < x + w && yy >= y && yy < y + h) continue;
                fb_blend_pixel(xx, yy, black, alpha);
            }
    }
}

/* Vertical gradient whose TOP corners are rounded (for title bars) */
void fb_gradient_v_round(i32 x, i32 y, i32 w, i32 h, i32 r,
                         u8 r1, u8 g1, u8 b1, u8 r2, u8 g2, u8 b2) {
    if (h <= 0 || w <= 0) return;
    if (r < 0) r = 0;
    if (2 * r > w) r = w / 2;
    if (r > h) r = h;
    for (i32 j = 0; j < h; j++) {
        u32 t = h > 1 ? (u32)j * 256u / (u32)(h - 1) : 0;
        u8 cr = (u8)(r1 + ((i32)(r2 - r1) * (i32)t) / 256);
        u8 cg = (u8)(g1 + ((i32)(g2 - g1) * (i32)t) / 256);
        u8 cb = (u8)(b1 + ((i32)(b2 - b1) * (i32)t) / 256);
        u32 col = fb_rgb(cr, cg, cb);
        if (j >= r || !fb_aa) {
            fb_fill(x, y + j, w, 1, col);
            continue;
        }
        fb_fill(x + r, y + j, w - 2 * r, 1, col);
        for (i32 i = 0; i < r; i++) {
            u32 cov = corner_cov(i, j, r, r, r);
            fb_blend_pixel(x + i, y + j, col, cov);
            fb_blend_pixel(x + w - 1 - i, y + j, col, cov);
        }
    }
}

static void fill_unclipped(i32 x, i32 y, i32 w, i32 h, u32 c) {
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

void fb_fill(i32 x, i32 y, i32 w, i32 h, u32 c) {
    if (!fb_ok || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (i32)fb_w || y >= (i32)fb_h || w <= 0 || h <= 0) return;
    if (w > (i32)fb_w - x) w = (i32)fb_w - x;
    if (h > (i32)fb_h - y) h = (i32)fb_h - y;
    if (w <= 0 || h <= 0) return;

    if (!fb_clip_enabled) {
        fill_unclipped(x, y, w, h, c);
        return;
    }

    /* A frame may have several disjoint damaged rectangles. Intersect the
       primitive with each one so apps can draw once for the whole frame. */
    for (u32 i = 0; i < fb_clip_count; i++) {
        const fb_clip_rect_t *r = &fb_clip_rects[i];
        i32 x0 = x > r->x ? x : r->x;
        i32 y0 = y > r->y ? y : r->y;
        i32 x1 = x + w < r->x + r->w ? x + w : r->x + r->w;
        i32 y1 = y + h < r->y + r->h ? y + h : r->y + r->h;
        if (x1 > x0 && y1 > y0)
            fill_unclipped(x0, y0, x1 - x0, y1 - y0, c);
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

/* Rounded rectangle with anti-aliased corners (radius 6) */
void fb_round_fill(i32 x, i32 y, i32 w, i32 h, u32 c) {
    fb_round_fill_aa(x, y, w, h, 6, c);
}

/* ---------- text ---------- */

/* Coverage level 0..15 of glyph pixel (r,c). Bold is synthesized from
   the regular face by smearing ink one pixel to the right. */
static inline u32 cov_at(const u8 *g, i32 r, i32 c, int bold) {
    u8 packed = g[r * 4 + (c >> 1)];
    u32 a = (c & 1) ? (packed & 0x0F) : (packed >> 4);
    if (bold && c > 0) {
        u8 prev = g[r * 4 + ((c - 1) >> 1)];
        u32 pa = ((c - 1) & 1) ? (prev & 0x0F) : (prev >> 4);
        if (pa > a) a = pa;
    }
    return a;
}

/* Draw one glyph from the coverage font scaled by `scale` (1 = 8x16).
   bg = 0xFFFFFFFF means a transparent background (blend over pixels). */
static void glyph_cov(i32 x, i32 y, u8 ch, int bold, u32 fg, u32 bg, i32 scale) {
    const u8 *cov = &kv_font_aa[(u32)ch * 64];
    if (fb_bpp == 32 && scale == 1 && bg != 0xFFFFFFFFu &&
        x >= 0 && y >= 0 && x + 8 <= (i32)fb_w && y + 16 <= (i32)fb_h) {
        /* solid background in the 32-bit buffer; the per-pixel clip test
           keeps this fast even when damage splits the glyph box. The 16
           coverage levels are pre-blended once per glyph into a LUT, so
           the inner loop is just loads and stores. */
        u8 *row = fb_draw + (u32)y * fb_pitch + (u32)x * 4;
        u32 lut[16];
        int full = clip_contains_box(x, y, 8, 16);
        for (int i = 0; i < 16; i++) lut[i] = blend_rgb(fg, bg, (u32)i * 17u);
        for (i32 r = 0; r < 16; r++, row += fb_pitch) {
            u32 *p = (u32 *)row;
            for (i32 c = 0; c < 8; c++) {
                if (!full && !clip_has_pixel(x + c, y + r)) continue;
                p[c] = lut[cov_at(cov, r, c, bold)];
            }
        }
        return;
    }
    if (fb_bpp == 32 && scale == 1 && bg == 0xFFFFFFFFu &&
        r_size == 8 && g_size == 8 && b_size == 8 &&
        x >= 0 && y >= 0 && x + 8 <= (i32)fb_w && y + 16 <= (i32)fb_h) {
        /* transparent background: blend ink over the pixels below.
           Only ink pixels are touched, with plain channel arithmetic. */
        int full = clip_contains_box(x, y, 8, 16);
        int fr = (int)((fg >> 16) & 255), fg_ = (int)((fg >> 8) & 255),
            fb_ = (int)(fg & 255);
        u8 *row = fb_draw + (u32)y * fb_pitch + (u32)x * 4;
        for (i32 r = 0; r < 16; r++, row += fb_pitch) {
            u32 *p = (u32 *)row;
            for (i32 c = 0; c < 8; c++) {
                u32 a = cov_at(cov, r, c, bold) * 17u;
                if (!a) continue;
                if (!full && !clip_has_pixel(x + c, y + r)) continue;
                if (a == 255) { p[c] = fg; continue; }
                u32 bgc = p[c], ia = 255 - a;
                p[c] = fb_rgb((u8)((fr * (int)a + (int)((bgc >> 16) & 255) * (int)ia) / 255),
                              (u8)((fg_ * (int)a + (int)((bgc >> 8) & 255) * (int)ia) / 255),
                              (u8)((fb_ * (int)a + (int)(bgc & 255) * (int)ia) / 255));
            }
        }
        return;
    }
    for (i32 r = 0; r < 16 * scale; r++) {
        for (i32 c = 0; c < 8 * scale; c++) {
            u32 a = cov_at(cov, r / scale, c / scale, bold) * 17u;
            if (!a) {
                if (bg != 0xFFFFFFFFu && scale == 1) fb_pixel((u32)(x + c), (u32)(y + r), bg);
                continue;
            }
            if (bg != 0xFFFFFFFFu && a == 255) {
                fb_pixel((u32)(x + c), (u32)(y + r), fg);
            } else if (bg != 0xFFFFFFFFu) {
                fb_pixel((u32)(x + c), (u32)(y + r), blend_rgb(fg, bg, a));
            } else {
                fb_blend_pixel(x + c, y + r, fg, a);
            }
        }
    }
}

static void glyph_bmp(i32 x, i32 y, u8 ch, u32 fg, u32 bg) {
    const u8 *g = &kv_font8x16[(u32)ch * 16];

    /* Fast path: the glyph fits entirely on screen and the mode is
       32-bit. Neither per-pixel clipping nor address recomputation is
       needed here - we walk the row with a pointer. There is a lot of
       text on screen, so this loop noticeably affects overall speed. */
    if (fb_bpp == 32 && x >= 0 && y >= 0 &&
        x + 8 <= (i32)fb_w && y + 16 <= (i32)fb_h &&
        clip_contains_box(x, y, 8, 16)) {
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
            if (px < 0 || px >= (i32)fb_w || !clip_has_pixel(px, py)) continue;
            if (bits & (0x80 >> col)) put_raw((u32)px, (u32)py, fg);
            else if (bg != 0xFFFFFFFFu) put_raw((u32)px, (u32)py, bg);
        }
    }
}

/* A UTF-8 string */
/* A single CP866 character. bg = 0xFFFFFFFF means a transparent background.
   With anti-aliasing on, the coverage font is blended over the background;
   the legacy bitmap path stays as the fallback (and for the AA-off mode). */
void fb_glyph(i32 x, i32 y, u8 ch, u32 fg, u32 bg) {
    if (!fb_ok) return;
    if (fb_aa) {
        glyph_cov(x, y, ch, 0, fg, bg, 1);
        return;
    }
    glyph_bmp(x, y, ch, fg, bg);
}

static void text_impl(i32 x, i32 y, const char *s, u32 fg, u32 bg,
                      int bold, i32 scale) {
    if (!fb_ok) return;
    while (*s) {
        u32 cp = utf8_next(&s);
        u8 ch = cp866_from_unicode(cp);
        if (fb_aa || scale != 1) glyph_cov(x, y, ch, bold, fg, bg, scale);
        else glyph_bmp(x, y, ch, fg, bg);
        x += 8 * scale;
    }
}

void fb_text(i32 x, i32 y, const char *s, u32 fg, u32 bg) {
    text_impl(x, y, s, fg, bg, 0, 1);
}

void fb_text_bold(i32 x, i32 y, const char *s, u32 fg, u32 bg) {
    text_impl(x, y, s, fg, bg, 1, 1);
}

/* Big smooth text for headings: the coverage cell is drawn scale x scale */
void fb_text_scaled(i32 x, i32 y, const char *s, i32 scale, u32 fg, u32 bg) {
    if (scale < 1) scale = 1;
    text_impl(x, y, s, fg, bg, 0, scale);
}

/* A string centred inside an area of width w */
void fb_text_center(i32 x, i32 y, i32 w, const char *s, u32 fg, u32 bg) {
    i32 tw = (i32)utf8_len(s) * 8;
    fb_text(x + (w - tw) / 2, y, s, fg, bg);
}

void fb_clear(u32 c) { fb_fill(0, 0, (i32)fb_w, (i32)fb_h, c); }

/* Copy a rectangular region between two framebuffer-format buffers.
   This bypasses drawing clips: the GUI uses it to restore cached pixels. */
void fb_copy_rect(void *dst, const void *src, i32 x, i32 y, i32 w, i32 h) {
    if (!fb_ok || !dst || !src || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (i32)fb_w || y >= (i32)fb_h || w <= 0 || h <= 0) return;
    if (w > (i32)fb_w - x) w = (i32)fb_w - x;
    if (h > (i32)fb_h - y) h = (i32)fb_h - y;
    if (w <= 0 || h <= 0) return;

    u32 bytes_per_pixel = fb_bpp >> 3;
    u32 row_bytes = (u32)w * bytes_per_pixel;
    const u8 *s = (const u8 *)src + (u32)y * fb_pitch + (u32)x * bytes_per_pixel;
    u8 *d = (u8 *)dst + (u32)y * fb_pitch + (u32)x * bytes_per_pixel;
    for (i32 row = 0; row < h; row++) {
        memcpy(d, s, row_bytes);
        s += fb_pitch;
        d += fb_pitch;
    }
}

/* Present only a damaged rectangle from a back buffer. */
void fb_present_rect(const void *back, i32 x, i32 y, i32 w, i32 h) {
    if (!fb_ok || !back || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (i32)fb_w || y >= (i32)fb_h || w <= 0 || h <= 0) return;
    if (w > (i32)fb_w - x) w = (i32)fb_w - x;
    if (h > (i32)fb_h - y) h = (i32)fb_h - y;
    if (w <= 0 || h <= 0) return;

    u32 bytes_per_pixel = fb_bpp >> 3;
    u32 row_bytes = (u32)w * bytes_per_pixel;
    const u8 *s = (const u8 *)back + (u32)y * fb_pitch + (u32)x * bytes_per_pixel;
    u8 *d = fb_hw + (u32)y * fb_pitch + (u32)x * bytes_per_pixel;
    for (i32 row = 0; row < h; row++) {
        memcpy(d, s, row_bytes);
        s += fb_pitch;
        d += fb_pitch;
    }
}

/* Fast blit of the back buffer to the screen */
/* Blit only a part of the frame (the band of rows y0..y1).
   The GUI uses this for legacy full-width updates; normal redraws use
   fb_present_rect() to transfer exactly the damaged pixels. */
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
