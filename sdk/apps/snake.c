/* Змейка - пример игры для KvantOS
   KAPP_NAME "Змейка"
   Стрелки - поворот, пробел - заново. */
#include "kvapp.h"
#define W 20
#define H 14
#define CELL 16
static const kv_api_t *sys;
static int sx[W*H], sy[W*H], slen, dx, dy, fx, fy, dead, score;
static kv_u32 last_step;

static void place_food(void) {
    for (int guard = 0; guard < 200; guard++) {
        int x = (int)(sys->random() % W), y = (int)(sys->random() % H);
        int busy = 0;
        for (int i = 0; i < slen; i++) if (sx[i]==x && sy[i]==y) busy = 1;
        if (!busy) { fx = x; fy = y; return; }
    }
    fx = 0; fy = 0;
}
static void restart(void) {
    slen = 3; dx = 1; dy = 0; dead = 0; score = 0;
    for (int i = 0; i < slen; i++) { sx[i] = 5 - i; sy[i] = 7; }
    place_food();
    last_step = sys->ticks();
    sys->status("Стрелки - поворот, пробел - заново");
}
static void step_game(void) {
    if (dead) return;
    int nx = sx[0] + dx, ny = sy[0] + dy;
    if (nx < 0 || ny < 0 || nx >= W || ny >= H) { dead = 1; sys->beep(200,120); return; }
    for (int i = 0; i < slen; i++)
        if (sx[i]==nx && sy[i]==ny) { dead = 1; sys->beep(200,120); return; }
    for (int i = slen; i > 0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
    sx[0] = nx; sy[0] = ny;
    if (nx==fx && ny==fy) {
        if (slen < W*H-1) slen++;
        score++; sys->beep(1400,30); place_food();
    }
}
static void on_tick(void) {
    kv_u32 hz = sys->hz(), now = sys->ticks();
    if (hz && now - last_step >= hz/6) { last_step = now; step_game(); }
}
static void on_draw(void) {
    kv_u32 bg = sys->rgb(24,32,44), grid = sys->rgb(34,44,60);
    kv_u32 head = sys->rgb(120,220,140), body = sys->rgb(60,170,100);
    kv_u32 food = sys->rgb(230,90,80), white = sys->rgb(235,240,248);
    sys->clear(bg);
    for (int x = 0; x <= W; x++) sys->line(x*CELL, 0, x*CELL, H*CELL, grid);
    for (int y = 0; y <= H; y++) sys->line(0, y*CELL, W*CELL, y*CELL, grid);
    sys->fill(fx*CELL+3, fy*CELL+3, CELL-6, CELL-6, food);
    for (int i = 0; i < slen; i++)
        sys->fill(sx[i]*CELL+2, sy[i]*CELL+2, CELL-4, CELL-4, i ? body : head);
    char buf[48];
    sys->format(buf, sizeof(buf), "Счёт: %u", (kv_u32)score);
    sys->text(6, H*CELL+6, buf, white, 0xFFFFFFFF);
    if (dead) sys->text(W*CELL/2-60, H*CELL/2-8, "Игра окончена", white, sys->rgb(160,40,40));
}
static void on_key(kv_i32 k) {
    if (k==KV_KEY_LEFT  && dx==0) { dx=-1; dy=0; }
    else if (k==KV_KEY_RIGHT && dx==0) { dx=1; dy=0; }
    else if (k==KV_KEY_UP    && dy==0) { dx=0; dy=-1; }
    else if (k==KV_KEY_DOWN  && dy==0) { dx=0; dy=1; }
    else if (k==' ') restart();
}
static kv_app_t me = { .title="Змейка", .width=W*CELL, .height=H*CELL+26,
    .on_open=restart, .on_draw=on_draw, .on_key=on_key, .on_tick=on_tick };
kv_app_t *kapp_main(const kv_api_t *api){ sys=api; return &me; }
