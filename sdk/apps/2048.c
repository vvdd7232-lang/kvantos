/* 2048 - the sliding tiles puzzle
   KAPP_NAME "2048"
   Arrow keys slide the tiles, R restarts. KvantOS 2.0. */
#include "kvapp.h"

static const kv_api_t *sys;

#define N 4
#define CELL 70
#define GAP 8
#define W (N * CELL + (N + 1) * GAP)
#define H (W + 34)

static int grid[N][N];
static int score, best, over, seen2048;

static void spawn_tile(void) {
    int ex[N * N], ey[N * N], n = 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            if (!grid[y][x]) { ex[n] = x; ey[n] = y; n++; }
    if (!n) return;
    int i = (int)(sys->random() % (kv_u32)n);
    grid[ey[i]][ex[i]] = (sys->random() % 10) ? 2 : 4;
}

static void field_reset(void) {
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) grid[y][x] = 0;
    score = 0; over = 0;
    spawn_tile(); spawn_tile();
}

/* Slide one row to the left; returns 1 if anything moved */
static int slide_row(int *r, int *gain) {
    int t[N], n = 0;
    for (int i = 0; i < N; i++) if (r[i]) t[n++] = r[i];
    for (int i = 0; i + 1 < n; i++)
        if (t[i] == t[i + 1]) {
            t[i] *= 2;
            *gain += t[i];
            if (t[i] == 2048 && !seen2048) {
                seen2048 = 1;
                sys->beep(1400, 70); sys->beep(1800, 110);
            }
            for (int j = i + 1; j + 1 < n; j++) t[j] = t[j + 1];
            n--;
        }
    for (int i = n; i < N; i++) t[i] = 0;
    int moved = 0;
    for (int i = 0; i < N; i++) {
        if (r[i] != t[i]) moved = 1;
        r[i] = t[i];
    }
    return moved;
}

static void move(int dir) {   /* 0 left, 1 right, 2 up, 3 down */
    if (over) return;
    int moved = 0, gain = 0;
    for (int i = 0; i < N; i++) {
        int row[N];
        for (int j = 0; j < N; j++) {
            if (dir < 2) row[j] = grid[i][dir == 0 ? j : N - 1 - j];
            else         row[j] = grid[dir == 2 ? j : N - 1 - j][i];
        }
        if (slide_row(row, &gain)) moved = 1;
        for (int j = 0; j < N; j++) {
            if (dir < 2) grid[i][dir == 0 ? j : N - 1 - j] = row[j];
            else         grid[dir == 2 ? j : N - 1 - j][i] = row[j];
        }
    }
    if (!moved) return;
    score += gain;
    if (score > best) best = score;
    spawn_tile();
    /* stuck? */
    int stuck = 1;
    for (int y = 0; y < N && stuck; y++)
        for (int x = 0; x < N && stuck; x++) {
            if (!grid[y][x]) stuck = 0;
            if (x + 1 < N && grid[y][x] == grid[y][x + 1]) stuck = 0;
            if (y + 1 < N && grid[y][x] == grid[y + 1][x]) stuck = 0;
        }
    if (stuck) { over = 1; sys->beep(160, 180); }
}

static void on_key(kv_i32 k) {
    if (k == 'r' || k == 'R') { field_reset(); return; }
    if (k == KV_KEY_LEFT) move(0);
    else if (k == KV_KEY_RIGHT) move(1);
    else if (k == KV_KEY_UP) move(2);
    else if (k == KV_KEY_DOWN) move(3);
}

static void tile_colors(int v, kv_u32 *bg, kv_u32 *fg) {
    switch (v) {
        case 2:    *bg = sys->rgb(238, 228, 218); *fg = sys->rgb(60, 50, 40); break;
        case 4:    *bg = sys->rgb(237, 224, 200); *fg = sys->rgb(60, 50, 40); break;
        case 8:    *bg = sys->rgb(242, 177, 121); *fg = sys->rgb(255, 250, 245); break;
        case 16:   *bg = sys->rgb(245, 149, 99);  *fg = sys->rgb(255, 250, 245); break;
        case 32:   *bg = sys->rgb(246, 124, 95);  *fg = sys->rgb(255, 250, 245); break;
        case 64:   *bg = sys->rgb(246, 94, 59);   *fg = sys->rgb(255, 250, 245); break;
        case 128:  *bg = sys->rgb(237, 207, 114); *fg = sys->rgb(255, 250, 245); break;
        case 256:  *bg = sys->rgb(237, 204, 97);  *fg = sys->rgb(255, 250, 245); break;
        case 512:  *bg = sys->rgb(237, 200, 80);  *fg = sys->rgb(255, 250, 245); break;
        case 1024: *bg = sys->rgb(237, 197, 63);  *fg = sys->rgb(255, 250, 245); break;
        default:   *bg = sys->rgb(237, 194, 46);  *fg = sys->rgb(255, 250, 245); break;
    }
}

static void on_draw(void) {
    sys->clear(sys->rgb(250, 248, 239));
    sys->fill(0, 0, W, W, sys->rgb(187, 173, 160));
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            int px = GAP + x * (CELL + GAP), py = GAP + y * (CELL + GAP);
            int v = grid[y][x];
            if (!v) {
                sys->fill(px, py, CELL, CELL, sys->rgb(205, 193, 180));
                continue;
            }
            kv_u32 bg, fg;
            tile_colors(v, &bg, &fg);
            sys->fill(px, py, CELL, CELL, bg);
            char s[8];
            sys->format(s, sizeof(s), "%d", v);
            int tw = sys->text_width(s);
            sys->text(px + (CELL - tw) / 2, py + CELL / 2 - 7, s, fg, 0xFFFFFFFF);
        }
    char msg[80];
    sys->format(msg, sizeof(msg), KV_T(sys, "Score: %d   Best: %d", "Счёт: %d   Рекорд: %d"), score, best);
    if (over) sys->format(msg, sizeof(msg), KV_T(sys, "Game over: %d. R to restart", "Игра окончена: %d. R - заново"), score);
    sys->text(4, W + 8, msg, sys->rgb(90, 80, 70), 0xFFFFFFFF);
    sys->status(KV_T(sys, "Arrows slide the tiles, R restarts",
                     "Стрелки двигают плитки, R - заново"));
}

static void on_open(void) { field_reset(); }

static kv_app_t me = {
    .title = "2048",
    .width = W, .height = H,
    .on_open = on_open, .on_draw = on_draw, .on_key = on_key,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    me.title = "2048";
    return &me;
}
