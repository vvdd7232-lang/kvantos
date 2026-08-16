/* ============================================================
 *  KvantOS - текстовая консоль.
 *  Работает в двух режимах: классический VGA 80x25 (0xB8000)
 *  и программная отрисовка глифов в линейный фреймбуфер.
 *  Строка 0 зарезервирована под статус-бар.
 * ============================================================ */
#include "kernel.h"

#define VGA_MEM   ((volatile u16 *)0xB8000)
#define TOP_ROW   1

extern const u8 kv_font8x16[256 * 16];

static u8  color = VGA_COLOR(VGA_LGREY, VGA_BLACK);
static u32 row = TOP_ROW, col = 0;
static u32 cols = 80, rows = 25;     /* размер сетки символов */
static int gfx = 0;                  /* 1 - рисуем в фреймбуфер */

/* Палитра VGA в RGB для графического режима */
static const u8 pal[16][3] = {
    {  0,  0,  0}, {  0,  0,170}, {  0,170,  0}, {  0,170,170},
    {170,  0,  0}, {170,  0,170}, {170, 85,  0}, {170,170,170},
    { 85, 85, 85}, { 85, 85,255}, { 85,255, 85}, { 85,255,255},
    {255, 85, 85}, {255, 85,255}, {255,255, 85}, {255,255,255}
};

static inline u16 cell(char c, u8 cl) { return (u16)(u8)c | ((u16)cl << 8); }
static inline u32 fg_rgb(u8 cl) { const u8 *p = pal[cl & 0x0F]; return fb_rgb(p[0], p[1], p[2]); }
static inline u32 bg_rgb(u8 cl) { const u8 *p = pal[(cl >> 4) & 0x0F]; return fb_rgb(p[0], p[1], p[2]); }

/* ---- загрузка пользовательского шрифта в плоскость 2 видеопамяти ---- */
static void vga_load_font(const u8 *font) {
    u32 fl = irq_save();
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);
    outb(0x3C4, 0x04); outb(0x3C5, 0x07);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
    outb(0x3CE, 0x06); outb(0x3CF, 0x00);

    volatile u8 *vram = (volatile u8 *)0xA0000;
    for (u32 ch = 0; ch < 256; ch++)
        for (u32 y = 0; y < 16; y++)
            vram[ch * 32 + y] = font[ch * 16 + y];

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x04); outb(0x3C5, 0x03);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);
    irq_restore(fl);
}

void vga_set_color(u8 c) { color = c; }
u8   vga_get_color(void) { return color; }

/* ---- курсор ---- */
static void cursor_hw(void) {
    u16 pos = (u16)(row * cols + col);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

static void cursor_gfx(int show) {
    u32 x = col * 8, y = row * 16;
    fb_fill((i32)x, (i32)(y + 14), 8, 2, show ? fg_rgb(color) : bg_rgb(color));
}

void vga_cursor_update(void) {
    if (gfx) cursor_gfx(1);
    else cursor_hw();
}

static void cursor_enable(void) {
    outb(0x3D4, 0x0A); outb(0x3D5, (inb(0x3D5) & 0xC0) | 13);
    outb(0x3D4, 0x0B); outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
}

/* ---- очистка / прокрутка ---- */
void vga_clear(void) {
    u32 fl = irq_save();
    if (gfx) {
        fb_fill(0, (i32)(TOP_ROW * 16), (i32)fb_width(),
                (i32)(fb_height() - TOP_ROW * 16), bg_rgb(color));
    } else {
        for (u32 y = TOP_ROW; y < rows; y++)
            for (u32 x = 0; x < cols; x++)
                VGA_MEM[y * cols + x] = cell(' ', color);
    }
    row = TOP_ROW; col = 0;
    vga_cursor_update();
    irq_restore(fl);
}

static void scroll(void) {
    if (gfx) {
        fb_scroll_up(TOP_ROW * 16, rows * 16, 16, bg_rgb(color));
    } else {
        for (u32 y = TOP_ROW; y < rows - 1; y++)
            for (u32 x = 0; x < cols; x++)
                VGA_MEM[y * cols + x] = VGA_MEM[(y + 1) * cols + x];
        for (u32 x = 0; x < cols; x++)
            VGA_MEM[(rows - 1) * cols + x] = cell(' ', color);
    }
    row = rows - 1;
}

/* ---- вывод символа CP866 ---- */
static void put_cell(u32 x, u32 y, u8 ch) {
    if (gfx) fb_glyph((i32)(x * 8), (i32)(y * 16), ch, fg_rgb(color), bg_rgb(color));
    else VGA_MEM[y * cols + x] = cell((char)ch, color);
}

static void vga_putb(u8 c) {
    u32 fl = irq_save();
    if (gfx) cursor_gfx(0);              /* стереть курсор */
    switch (c) {
        case '\n': col = 0; row++; break;
        case '\r': col = 0; break;
        case '\t': col = (col + 8) & ~7u; break;
        case '\b':
            if (col) col--;
            else if (row > TOP_ROW) { row--; col = cols - 1; }
            put_cell(col, row, ' ');
            break;
        default:
            if (c < 32 && c != 0x01 && c != 0x03 && c != 0x04 && c != 0x07
                && c != 0x0F && c != 0x10 && c != 0x11
                && c != 0x18 && c != 0x19 && c != 0x1A && c != 0x1B) break;
            put_cell(col, row, c);
            col++;
    }
    if (col >= cols) { col = 0; row++; }
    while (row >= rows) scroll();
    vga_cursor_update();
    irq_restore(fl);
}

/* Потоковый декодер UTF-8 */
void vga_putc(char ch) {
    static u32 pending = 0;
    static int need = 0;
    u8 c = (u8)ch;

    if (need) {
        if ((c & 0xC0) == 0x80) {
            pending = (pending << 6) | (c & 0x3F);
            if (--need == 0) vga_putb(cp866_from_unicode(pending));
            return;
        }
        need = 0;
    }
    if (c < 0x80)           { vga_putb(c); return; }
    if ((c & 0xE0) == 0xC0) { pending = c & 0x1F; need = 1; return; }
    if ((c & 0xF0) == 0xE0) { pending = c & 0x0F; need = 2; return; }
    if ((c & 0xF8) == 0xF0) { pending = c & 0x07; need = 3; return; }
    vga_putb('?');
}

void vga_puts(const char *s) { while (*s) vga_putc(*s++); }

/* ---- статус-строка ---- */
void vga_status(const char *left, const char *right, u8 cl) {
    u8 lbuf[256], rbuf[256];
    u32 lim = cols < 256 ? cols : 255;
    u32 ln = left ? utf8_to_cp866(left, lbuf, lim) : 0;
    u32 rn = right ? utf8_to_cp866(right, rbuf, lim) : 0;

    u32 fl = irq_save();
    if (gfx) {
        u32 bg = bg_rgb(cl), fg = fg_rgb(cl);
        fb_fill(0, 0, (i32)fb_width(), 16, bg);
        for (u32 x = 0; x < ln && x < cols; x++)
            fb_glyph((i32)(x * 8), 0, lbuf[x], fg, bg);
        if (rn && rn + 1 < cols)
            for (u32 x = 0; x < rn; x++)
                fb_glyph((i32)((cols - rn - 1 + x) * 8), 0, rbuf[x], fg, bg);
    } else {
        for (u32 x = 0; x < cols; x++) VGA_MEM[x] = cell(' ', cl);
        for (u32 x = 0; x < ln && x < cols; x++) VGA_MEM[x] = cell((char)lbuf[x], cl);
        if (rn && rn + 1 < cols) {
            u32 off = cols - rn - 1;
            for (u32 x = 0; x < rn; x++) VGA_MEM[off + x] = cell((char)rbuf[x], cl);
        }
    }
    irq_restore(fl);
}

void vga_panic_screen(void) {
    color = VGA_COLOR(VGA_WHITE, VGA_RED);
    if (gfx) {
        fb_set_target(NULL);          /* задний буфер мог быть освобождён */
        /* после смены разрешения сетка символов могла устареть */
        cols = fb_width() / 8;
        rows = fb_height() / 16;
        fb_clear(bg_rgb(color));
    } else {
        for (u32 i = 0; i < cols * rows; i++) VGA_MEM[i] = cell(' ', color);
    }
    row = TOP_ROW; col = 0;
}

/* Вернуться в консоль после графической оболочки */
void vga_text_mode_restore(void) {
    if (gfx) {
        fb_set_target(NULL);
        fb_clear(bg_rgb(color));
    }
    row = TOP_ROW; col = 0;
    vga_cursor_update();
}

/* Пересчитать сетку символов после смены видеорежима */
void fb_console_resync(void) {
    if (!fb_active()) return;
    gfx  = 1;
    cols = fb_width() / 8;
    rows = fb_height() / 16;
    if (row >= rows) row = rows - 1;
    if (col >= cols) col = cols - 1;
    fb_clear(bg_rgb(color));
    row = TOP_ROW; col = 0;
    vga_cursor_update();
}

u32 vga_cols(void) { return cols; }
u32 vga_rows(void) { return rows; }
int vga_is_gfx(void) { return gfx; }

void vga_init(void) {
    color = VGA_COLOR(VGA_LGREY, VGA_BLACK);

    if (fb_active()) {
        gfx  = 1;
        cols = fb_width() / 8;
        rows = fb_height() / 16;
        fb_clear(bg_rgb(color));
    } else {
        gfx  = 0;
        cols = 80; rows = 25;
        vga_load_font(kv_font8x16);
        cursor_enable();
        for (u32 i = 0; i < cols * rows; i++) VGA_MEM[i] = cell(' ', color);
    }
    row = TOP_ROW; col = 0;
    vga_cursor_update();
}
