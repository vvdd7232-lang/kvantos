/* Paint - draw with the mouse
   KAPP_NAME "Paint"
   Left-click stamps the brush; the palette is on top. Keys 1-8 pick a
   colour, E is the eraser, C clears the canvas, B toggles the brush
   size. */
#include "kvapp.h"

static const kv_api_t *sys;

#define TOOL_H  32
#define SWATCH  26
#define CANVAS_W 480
#define CANVAS_H 320

static kv_u32 palette[8];
static kv_u32 c_bg, c_frame, c_text, c_mark;
static int cur_color;          /* 0..7, -1 = eraser */
static int brush_big;          /* 0 = small brush, 1 = big */
static kv_u32 clicks;

static void recolor(void) {
    c_bg    = sys->rgb(36, 40, 50);
    c_frame = sys->rgb(90, 98, 112);
    c_text  = sys->rgb(228, 232, 240);
    c_mark  = sys->rgb(255, 210, 80);
}

static void make_palette(void) {
    palette[0] = sys->rgb(235, 238, 244);   /* white   */
    palette[1] = sys->rgb(230,  80,  70);   /* red     */
    palette[2] = sys->rgb(255, 170,  60);   /* orange  */
    palette[3] = sys->rgb(250, 220,  80);   /* yellow  */
    palette[4] = sys->rgb( 90, 190, 110);   /* green   */
    palette[5] = sys->rgb( 80, 150, 230);   /* blue    */
    palette[6] = sys->rgb(170, 110, 220);   /* violet  */
    palette[7] = sys->rgb( 24, 26, 32);     /* eraser  */
}

static void stamp(kv_i32 x, kv_i32 y) {
    kv_u32 color = (cur_color < 0) ? palette[7] : palette[cur_color];
    int r = brush_big ? 6 : 2;
    sys->fill(x - r, y - r, r * 2 + 1, r * 2 + 1, color);
    clicks++;
}

static void on_draw(void) {
    sys->clear(c_bg);

    /* the palette row */
    for (int i = 0; i < 8; i++) {
        kv_i32 x = 8 + i * (SWATCH + 6);
        sys->fill(x, 6, SWATCH, TOOL_H - 12, palette[i]);
        sys->rect(x, 6, SWATCH, TOOL_H - 12,
                  (i == cur_color) ? c_mark : c_frame);
        if (i == cur_color) sys->rect(x - 2, 4, SWATCH + 4, TOOL_H - 8, c_mark);
    }
    /* the clear field */
    kv_i32 cx = 8 + 8 * (SWATCH + 6) + 10;
    sys->fill(cx, 6, 90, TOOL_H - 12, sys->rgb(70, 76, 90));
    sys->rect(cx, 6, 90, TOOL_H - 12, c_frame);
    sys->text(cx + 8, 6 + (TOOL_H - 12 - KV_CHAR_H) / 2,
              KV_T(sys, "Clear", "Очистить"), c_text, sys->rgb(70, 76, 90));

    /* the canvas */
    sys->fill(8, TOOL_H + 4, CANVAS_W, CANVAS_H, palette[7]);
    sys->rect(8, TOOL_H + 4, CANVAS_W, CANVAS_H, c_frame);

    char buf[64];
    sys->format(buf, sizeof(buf), KV_T(sys, "Clicks: %u", "Кликов: %u"), clicks);
    sys->text(CANVAS_W - sys->text_width(buf) + 8, TOOL_H + CANVAS_H + 10, buf, c_text, 0xFFFFFFFF);

    sys->status(KV_T(sys, "Click to draw. 1-8 colour, E eraser, C clear, B brush size",
                     "Клик - рисунок. 1-8 цвет, E ластик, C очистить, B размер кисти"));
}

static void on_click(kv_i32 x, kv_i32 y, kv_i32 button) {
    (void)button;
    /* palette swatches */
    if (y >= 4 && y < TOOL_H) {
        for (int i = 0; i < 8; i++) {
            kv_i32 sx = 8 + i * (SWATCH + 6);
            if (x >= sx && x < sx + SWATCH) { cur_color = i; return; }
        }
        kv_i32 cx = 8 + 8 * (SWATCH + 6) + 10;
        if (x >= cx && x < cx + 90) {
            sys->fill(8, TOOL_H + 4, CANVAS_W, CANVAS_H, palette[7]);
            return;
        }
        return;
    }
    /* canvas */
    if (y > TOOL_H + 4 && y < TOOL_H + 4 + CANVAS_H)
        stamp(x, y);
}

static void on_key(kv_i32 k) {
    if (k >= '1' && k <= '8') cur_color = k - '1';
    else if (k == 'E' || k == 'e') cur_color = -1;
    else if (k == 'B' || k == 'b') brush_big = !brush_big;
    else if (k == 'C' || k == 'c')
        sys->fill(8, TOOL_H + 4, CANVAS_W, CANVAS_H, palette[7]);
}

static kv_app_t me = {
    .title = "Paint",
    .width = CANVAS_W + 16,
    .height = TOOL_H + CANVAS_H + 30,
    .on_open = 0, .on_draw = on_draw, .on_key = on_key, .on_click = on_click,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    recolor();
    make_palette();
    cur_color = 1;
    brush_big = 0;
    clicks = 0;
    me.title = KV_T(sys, "Paint", "Рисование");
    return &me;
}
