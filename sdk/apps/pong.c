/* Pong - you against the machine
   KAPP_NAME "Pong"
   Up/Down (or W/S) move the left bat, the right one is the computer.
   Space serves the ball and restarts after a game. First to 7 wins. */
#include "kvapp.h"

static const kv_api_t *sys;

#define W 480
#define H 320
#define PAD_H 56
#define PAD_W 10
#define WIN 7

static int py, ay;                 /* player and AI bat tops */
static int bx, by, vx, vy;         /* the ball */
static int ps, as;                 /* scores */
static int served, over;
static kv_u32 last_move;

static void serve(int to_player) {
    bx = W / 2; by = H / 2;
    vx = to_player ? -3 : 3;
    vy = ((int)(sys->random() % 5)) - 2;
    if (vy == 0) vy = 1;
    served = 1;
}

static void restart(void) {
    ps = 0; as = 0; over = 0;
    py = (H - PAD_H) / 2; ay = py;
    served = 0;
    sys->status(KV_T(sys, "Space to serve. Up/Down or W/S to move",
                     "Пробел - подача. Вверх/вниз или W/S - движение"));
}

static void move_ai(void) {
    /* the AI follows the ball with a dead zone so it can be beaten */
    int mid = ay + PAD_H / 2;
    if (vx > 0) {
        if (by < mid - 14) ay -= 2;
        else if (by > mid + 14) ay += 2;
    } else {
        if (mid < H / 2 - 10) ay += 1;
        else if (mid > H / 2 + 10) ay -= 1;
    }
    if (ay < 0) ay = 0;
    if (ay > H - PAD_H) ay = H - PAD_H;
}

static void on_tick(void) {
    kv_u32 hz = sys->hz(), now = sys->ticks();
    if (!served || over) return;
    if (hz && now - last_move >= hz / 60) {
        last_move = now;
        bx += vx; by += vy;
        if (by < 4) { by = 4; vy = -vy; }
        if (by > H - 4) { by = H - 4; vy = -vy; }

        /* player bat */
        if (vx < 0 && bx <= 24 + PAD_W && bx >= 24 && by >= py && by <= py + PAD_H) {
            vx = -vx; if (vx < 7) vx++;
            vy += (by - (py + PAD_H / 2)) / 16;
            sys->beep(700, 20);
        }
        /* AI bat */
        if (vx > 0 && bx >= W - 24 - PAD_W && bx <= W - 24 && by >= ay && by <= ay + PAD_H) {
            vx = -vx; if (vx > -7) vx--;
            vy += (by - (ay + PAD_H / 2)) / 16;
            sys->beep(500, 20);
        }
        if (vy > 7) vy = 7;
        if (vy < -7) vy = -7;

        /* a point */
        if (bx < -8) { as++; sys->beep(220, 90); if (as >= WIN) over = 1; else serve(1); }
        if (bx > W + 8) { ps++; sys->beep(1200, 60); if (ps >= WIN) over = 1; else serve(0); }

        move_ai();
    }
}

static void on_draw(void) {
    kv_u32 bg = sys->rgb(16, 20, 28), line = sys->rgb(56, 64, 80);
    kv_u32 white = sys->rgb(230, 234, 242), red = sys->rgb(220, 90, 80);
    sys->clear(bg);

    /* the net */
    for (int y = 0; y < H; y += 16) sys->fill(W / 2 - 1, y, 2, 8, line);

    /* bats and ball */
    sys->fill(24, py, PAD_W, PAD_H, white);
    sys->fill(W - 24 - PAD_W, ay, PAD_W, PAD_H, red);
    if (served && !over) sys->fill(bx - 4, by - 4, 8, 8, white);

    /* scores */
    char buf[16];
    sys->format(buf, sizeof(buf), "%d", ps);
    sys->text(W / 2 - 40, 10, buf, white, 0xFFFFFFFF);
    sys->format(buf, sizeof(buf), "%d", as);
    sys->text(W / 2 + 28, 10, buf, red, 0xFFFFFFFF);

    if (!served && !over)
        sys->text(W / 2 - 90, H / 2 - 8, KV_T(sys, "Space to serve", "Пробел - подача"), white, 0xFFFFFFFF);
    if (over) {
        const char *msg = ps > as ? KV_T(sys, "You win! Space restarts", "Вы победили! Пробел - заново")
                                  : KV_T(sys, "The machine wins. Space restarts", "Машина победила. Пробел - заново");
        sys->text(W / 2 - sys->text_width(msg) / 2, H / 2 - 8, msg, white, sys->rgb(60, 30, 90));
    }
}

static void on_key(kv_i32 k) {
    int step = 18;
    if (k == KV_KEY_UP || k == 'W' || k == 'w') py -= step;
    else if (k == KV_KEY_DOWN || k == 'S' || k == 's') py += step;
    else if (k == ' ') { if (!served || over) { if (over) restart(); serve(as > ps); } }
    if (py < 0) py = 0;
    if (py > H - PAD_H) py = H - PAD_H;
}

static kv_app_t me = {
    .title = "Pong",
    .width = W, .height = H + 24,
    .on_open = restart, .on_draw = on_draw, .on_key = on_key, .on_tick = on_tick,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    me.title = KV_T(sys, "Pong", "Понг");
    return &me;
}
