/* Life - Conway's Game of Life
   KAPP_NAME "Life"
   Click toggles cells. Space steps, G runs and stops, R fills a random
   field, C clears. The generation counter is at the bottom. */
#include "kvapp.h"

static const kv_api_t *sys;

#define GW 60
#define GH 38
#define CELL 8
#define FPS_MS 120

static unsigned char grid[GH][GW];
static unsigned char next[GH][GW];
static int running;
static kv_u32 gen;
static kv_u32 last_step;

static void clear_grid(void) {
    sys->mem_set(grid, 0, sizeof(grid));
    gen = 0;
}

static void random_grid(void) {
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            grid[y][x] = (unsigned char)(sys->random() % 100 < 22);
    gen = 0;
}

static void do_step(void) {
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int nx = (x + dx + GW) % GW;
                    int ny = (y + dy + GH) % GH;
                    n += grid[ny][nx];
                }
            }
            next[y][x] = (unsigned char)(grid[y][x] ? (n == 2 || n == 3) : (n == 3));
        }
    }
    sys->mem_copy(grid, next, sizeof(grid));
    gen++;
}

static void on_tick(void) {
    kv_u32 now = sys->ticks(), hz = sys->hz();
    if (running && hz && now - last_step >= hz * FPS_MS / 1000) {
        last_step = now;
        do_step();
    }
}

static void on_draw(void) {
    kv_u32 bg = sys->rgb(18, 22, 30), line = sys->rgb(34, 40, 52);
    kv_u32 alive = sys->rgb(96, 210, 130), head = sys->rgb(200, 240, 255);
    sys->clear(bg);

    for (int x = 0; x <= GW; x++) sys->line(x * CELL, 0, x * CELL, GH * CELL, line);
    for (int y = 0; y <= GH; y++) sys->line(0, y * CELL, GW * CELL, y * CELL, line);

    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            if (grid[y][x]) sys->fill(x * CELL + 1, y * CELL + 1, CELL - 2, CELL - 2, alive);

    char buf[64];
    sys->format(buf, sizeof(buf), KV_T(sys, "Generation: %u%s", "Поколение: %u%s"), gen,
                running ? "" : KV_T(sys, "  (paused)", "  (пауза)"));
    sys->text(6, GH * CELL + 6, buf, head, 0xFFFFFFFF);

    sys->status(KV_T(sys, "Click: cell. Space: step, G: run, R: random, C: clear",
                     "Клик: клетка. Пробел: шаг, G: пуск, R: случайное поле, C: очистить"));
}

static void on_click(kv_i32 x, kv_i32 y, kv_i32 button) {
    (void)button;
    int gx = x / CELL, gy = y / CELL;
    if (gx < 0 || gy < 0 || gx >= GW || gy >= GH) return;
    grid[gy][gx] = (unsigned char)!grid[gy][gx];
}

static void on_key(kv_i32 k) {
    if (k == ' ') { if (!running) do_step(); }
    else if (k == 'G' || k == 'g') { running = !running; last_step = sys->ticks(); }
    else if (k == 'R' || k == 'r') random_grid();
    else if (k == 'C' || k == 'c') clear_grid();
}

static void on_open(void) {
    random_grid();
    running = 1;
    last_step = sys->ticks();
}

static kv_app_t me = {
    .title = "Life",
    .width = GW * CELL, .height = GH * CELL + 28,
    .on_open = on_open, .on_draw = on_draw, .on_key = on_key,
    .on_click = on_click, .on_tick = on_tick,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    me.title = KV_T(sys, "Life", "Жизнь");
    return &me;
}
