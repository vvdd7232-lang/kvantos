/* ============================================================
 *  Clock - a sample application for KvantOS
 *  KAPP_NAME "Clock"
 *
 *  Demonstrates everything essential about KvApp:
 *    - how to declare the kapp_main entry point;
 *    - how to keep the system function table;
 *    - how to draw in on_draw and react to keys.
 *
 *  Build:  cd sdk && make
 *  Result: sdk/build/clock.kapp
 * ============================================================ */
#include "kvapp.h"

/* The system function table. It is stored at start-up: an
   application has no other way to reach the system. */
static const kv_api_t *sys;

/* Settings the user can change */
static int show_seconds = 1;
static int dark_theme   = 0;

/* Pre-computed colours: calling rgb() for every pixel is pointless */
static kv_u32 col_bg, col_face, col_hand, col_sec, col_text, col_mark;

static void recolor(void) {
    if (dark_theme) {
        col_bg   = sys->rgb(24, 28, 38);
        col_face = sys->rgb(38, 44, 58);
        col_mark = sys->rgb(120, 132, 155);
        col_text = sys->rgb(226, 232, 240);
    } else {
        col_bg   = sys->rgb(244, 246, 250);
        col_face = sys->rgb(255, 255, 255);
        col_mark = sys->rgb(150, 160, 178);
        col_text = sys->rgb(30, 38, 52);
    }
    col_hand = sys->rgb(38, 92, 168);
    col_sec  = sys->rgb(200, 60, 60);
}

/* --- a little trigonometry without a maths library ---
   A sine table for a quarter turn in 16 steps: enough to make the
   hands look straight. The values are multiplied by 1024. */
static const int sin_q[17] = {
       0,  100,  200,  297,  391,  482,  568,  650,
     724,  792,  851,  903,  946,  978, 1004, 1019, 1024
};

/* Sine of an angle given in 1/60 of a turn (handy for clock hands) */
static int sin60(int units) {
    units = ((units % 60) + 60) % 60;
    int quarter = units / 15, rest = units % 15;
    int idx = rest * 16 / 15;
    switch (quarter) {
        case 0: return  sin_q[idx];
        case 1: return  sin_q[16 - idx];
        case 2: return -sin_q[idx];
        default: return -sin_q[16 - idx];
    }
}
static int cos60(int units) { return sin60(units + 15); }

/* A hand from the centre: drawn three lines thick so it reads well */
static void hand(int cx, int cy, int units, int len, kv_u32 color, int thick) {
    int dx = sin60(units) * len / 1024;
    int dy = -cos60(units) * len / 1024;
    sys->line(cx, cy, cx + dx, cy + dy, color);
    if (thick) {
        sys->line(cx + 1, cy, cx + dx + 1, cy + dy, color);
        sys->line(cx, cy + 1, cx + dx, cy + dy + 1, color);
    }
}

static void on_draw(void) {
    int w = sys->width(), h = sys->height();
    sys->clear(col_bg);

    int cx = w / 2, cy = (h - 28) / 2 + 8;
    int radius = (cx < cy ? cx : cy) - 14;
    if (radius < 30) radius = 30;

    /* the dial */
    for (int r = radius; r > radius - 3; r--) {
        for (int a = 0; a < 60; a++) {
            int px = cx + sin60(a) * r / 1024;
            int py = cy - cos60(a) * r / 1024;
            sys->pixel(px, py, col_mark);
        }
    }
    /* filling the circle from inside - roughly, in bands */
    for (int y = -radius + 3; y < radius - 2; y++) {
        int half = 0;
        while (half * half + y * y < (radius - 3) * (radius - 3)) half++;
        sys->fill(cx - half, cy + y, half * 2, 1, col_face);
    }

    /* hour marks */
    for (int a = 0; a < 60; a += 5) {
        int r1 = radius - 8, r2 = radius - 4;
        sys->line(cx + sin60(a) * r1 / 1024, cy - cos60(a) * r1 / 1024,
                  cx + sin60(a) * r2 / 1024, cy - cos60(a) * r2 / 1024, col_mark);
    }

    kv_i32 hh = 0, mm = 0, ss = 0;
    sys->clock(&hh, &mm, &ss);

    /* the hour hand moves smoothly: 5 divisions per hour plus a share of the minutes */
    hand(cx, cy, (hh % 12) * 5 + mm / 12, radius / 2,     col_hand, 1);
    hand(cx, cy, mm,                      radius * 3 / 4, col_hand, 1);
    if (show_seconds)
        hand(cx, cy, ss,                  radius * 4 / 5, col_sec,  0);

    /* the axis */
    sys->fill(cx - 2, cy - 2, 5, 5, col_hand);

    /* the digital clock below */
    char buf[32];
    if (show_seconds) sys->format(buf, sizeof(buf), "%u:%u:%u", hh, mm, ss);
    else              sys->format(buf, sizeof(buf), "%u:%u", hh, mm);
    int tw = sys->text_width(buf);
    sys->text((w - tw) / 2, h - 20, buf, col_text, 0xFFFFFFFF);
}

static void on_key(kv_i32 key) {
    if (key == 's' || key == 'S') {
        show_seconds = !show_seconds;
        sys->status(show_seconds ? KV_T(sys, "Second hand on", "Секундная стрелка включена") : KV_T(sys, "Second hand off", "Секундная стрелка выключена"));
    } else if (key == 'd' || key == 'D') {
        dark_theme = !dark_theme;
        recolor();
        sys->status(dark_theme ? KV_T(sys, "Dark theme", "Тёмная тема") : KV_T(sys, "Light theme", "Светлая тема"));
    } else if (key == 'b' || key == 'B') {
        sys->beep(880, 60);
        sys->status(KV_T(sys, "Beep!", "Бип!"));
    }
}

static void on_click(kv_i32 x, kv_i32 y, kv_i32 button) {
    (void)x; (void)y; (void)button;
    dark_theme = !dark_theme;
    recolor();
    sys->status(dark_theme ? KV_T(sys, "Dark theme", "Тёмная тема") : KV_T(sys, "Light theme", "Светлая тема"));
}

static void on_open(void) {
    sys->status(KV_T(sys, "S - seconds, D - theme, B - beep", "S - секунды, D - тема, B - сигнал"));
    sys->log(KV_T(sys, "Clock: started", "Часы: запущены"));
}

/* The application description. It must outlive kapp_main, hence static. */
static kv_app_t me = {
    .title  = "Clock",   /* localised in kapp_main */
    .width  = 320,
    .height = 300,
    .on_open  = on_open,
    .on_draw  = on_draw,
    .on_key   = on_key,
    .on_click = on_click,
};

/* The entry point: the kernel calls it once at start-up */
kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    /* The window title cannot be set in a static initialiser:
       the language is only known at run time. */
    me.title = KV_T(sys, "Clock", "Часы");
    recolor();
    return &me;
}
