/* Stars - a starfield flight
   KAPP_NAME "Stars"
   A screensaver: the ship flies through a cloud of stars. + and -
   change the speed, Space pauses, Esc closes the window. */
#include "kvapp.h"

static const kv_api_t *sys;

#define NSTARS 160
#define W 480
#define H 320

static kv_i32 sx[NSTARS], sy[NSTARS], sz[NSTARS];
static int speed = 4;
static int paused;
static kv_u32 motion_accum;

static void new_star(int i, int far) {
    sx[i] = (kv_i32)(sys->random() % W) - W / 2;
    sy[i] = (kv_i32)(sys->random() % H) - H / 2;
    sz[i] = far ? (kv_i32)(100 + sys->random() % 400)
                : (kv_i32)(20 + sys->random() % 480);
}

static void on_open(void) {
    for (int i = 0; i < NSTARS; i++) new_star(i, 0);
    speed = 4;
    paused = 0;
    motion_accum = 0;
}

static void on_tick(void) {
    if (paused) { motion_accum = 0; return; }
    /* Preserve the old speed scale (distance per 16 ms) while allowing
       the GUI to render at 30, 60 or an uncapped frame rate. */
    motion_accum += (kv_u32)speed * sys->delta_ms();
    kv_u32 steps = motion_accum / 16;
    motion_accum %= 16;
    if (!steps) return;
    for (int i = 0; i < NSTARS; i++) {
        sz[i] -= (kv_i32)steps;
        if (sz[i] <= 2) new_star(i, 1);
    }
}

static void on_draw(void) {
    sys->clear(sys->rgb(4, 6, 12));
    int cx = W / 2, cy = H / 2;
    for (int i = 0; i < NSTARS; i++) {
        /* perspective projection */
        int px = cx + sx[i] * 260 / sz[i];
        int py = cy + sy[i] * 260 / sz[i];
        if (px < 0 || py < 0 || px >= W || py >= H) continue;
        int bright = 255 - sz[i] / 2;
        if (bright < 40) bright = 40;
        kv_u32 c = sys->rgb((kv_u8)bright, (kv_u8)bright, (kv_u8)(bright > 200 ? 255 : bright + 40));
        sys->pixel(px, py, c);
        if (sz[i] < 60) {          /* near stars get a little trail */
            sys->pixel(px + 1, py, c);
            sys->pixel(px, py + 1, c);
        }
    }
    char buf[48];
    sys->format(buf, sizeof(buf), KV_T(sys, "Speed %d%s", "Скорость %d%s"), speed,
                paused ? KV_T(sys, " (paused)", " (пауза)") : "");
    sys->text(8, H - 22, buf, sys->rgb(150, 170, 210), 0xFFFFFFFF);
    sys->status(KV_T(sys, "+/- speed, Space pause, Esc close",
                     "+/- скорость, пробел пауза, Esc закрыть"));
}

static void on_key(kv_i32 k) {
    if (k == '+' || k == '=') { if (speed < 20) speed++; }
    else if (k == '-') { if (speed > 1) speed--; }
    else if (k == ' ') paused = !paused;
}

static kv_app_t me = {
    .title = "Stars",
    .width = W, .height = H,
    .on_open = on_open, .on_draw = on_draw, .on_key = on_key, .on_tick = on_tick,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    me.title = KV_T(sys, "Stars", "Звёзды");
    return &me;
}
