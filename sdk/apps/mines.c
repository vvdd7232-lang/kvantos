/* Mines - Minesweeper
   KAPP_NAME "Mines"
   Left click or Enter opens a cell, F flags it, arrows move the
   cursor, R restarts. KvantOS 2.0. */
#include "kvapp.h"

static const kv_api_t *sys;

#define COLS 16
#define ROWS 12
#define MINES 24
#define CELL 26
#define W (COLS * CELL)
#define H (ROWS * CELL + 26)

static kv_u8 mines[ROWS][COLS];
static kv_u8 opened[ROWS][COLS];
static kv_u8 flagged[ROWS][COLS];
static int cx, cy;                 /* keyboard cursor           */
static int dead, won, revealed;    /* revealed = show all mines */
static kv_u32 started;             /* first click not yet made  */

static void field_reset(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            mines[y][x] = opened[y][x] = flagged[y][x] = 0;
    cx = COLS / 2; cy = ROWS / 2;
    dead = won = revealed = 0;
    started = 1;
}

/* Place MINES at random; the cell of the very first click stays safe */
static void place_mines(int sx, int sy) {
    int placed = 0, guard = 10000;
    while (placed < MINES && guard--) {
        int x = (int)(sys->random() % COLS);
        int y = (int)(sys->random() % ROWS);
        if (mines[y][x] || (x == sx && y == sy)) continue;
        mines[y][x] = 1;
        placed++;
    }
    started = 0;
}

static int around(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < COLS && ny < ROWS) n += mines[ny][nx];
        }
    return n;
}

static void check_win(void) {
    int open = 0;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) open += opened[y][x];
    if (open == ROWS * COLS - MINES) {
        won = 1; revealed = 1;
        sys->beep(1200, 60); sys->beep(1600, 90);
    }
}

static void open_cell(int x, int y) {
    if (dead || won || flagged[y][x] || opened[y][x]) return;
    if (started) place_mines(x, y);
    if (mines[y][x]) {
        dead = 1; revealed = 1;
        sys->beep(180, 160);
        return;
    }
    /* flood fill with an explicit stack */
    static kv_i32 qx[COLS * ROWS], qy[COLS * ROWS];
    int qh = 0, qt = 0;
    qx[qt] = (kv_i32)x; qy[qt] = (kv_i32)y; qt++;
    while (qh < qt) {
        int px = qx[qh], py = qy[qh]; qh++;
        if (px < 0 || py < 0 || px >= COLS || py >= ROWS) continue;
        if (opened[py][px] || flagged[py][px] || mines[py][px]) continue;
        opened[py][px] = 1;
        if (around(px, py) == 0)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    if (qt < COLS * ROWS) {
                        qx[qt] = (kv_i32)(px + dx);
                        qy[qt] = (kv_i32)(py + dy);
                        qt++;
                    }
                }
    }
    check_win();
}

static void on_open(void) { field_reset(); }

static void on_click(kv_i32 x, kv_i32 y, kv_i32 button) {
    (void)button;
    if (dead || won) return;
    int gx = x / CELL, gy = y / CELL;
    if (gx < 0 || gy < 0 || gx >= COLS || gy >= ROWS) return;
    cx = gx; cy = gy;
    open_cell(gx, gy);
}

static void on_key(kv_i32 k) {
    if (k == 'r' || k == 'R') { field_reset(); return; }
    if (dead || won) return;
    if (k == KV_KEY_LEFT)  { if (cx > 0) cx--; }
    else if (k == KV_KEY_RIGHT) { if (cx < COLS - 1) cx++; }
    else if (k == KV_KEY_UP)    { if (cy > 0) cy--; }
    else if (k == KV_KEY_DOWN)  { if (cy < ROWS - 1) cy++; }
    else if (k == 'f' || k == 'F' || k == 's' || k == 'S')
        flagged[cy][cx] = (kv_u8)!flagged[cy][cx];
    else if (k == ' ' || k == KV_KEY_ENTER)
        open_cell(cx, cy);
}

static void on_draw(void) {
    sys->clear(sys->rgb(46, 52, 64));
    static const kv_u8 ncol[9][3] = {
        {0,0,0}, {90,150,255}, {90,200,110}, {255,120,90}, {170,110,255},
        {255,170,80}, {90,220,220}, {255,255,255}, {200,200,200}
    };
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            int px = x * CELL, py = y * CELL;
            if (!opened[y][x] && !revealed) {
                sys->fill(px + 1, py + 1, CELL - 2, CELL - 2, sys->rgb(96, 108, 128));
                sys->fill(px + 1, py + 1, CELL - 2, 3, sys->rgb(128, 140, 160));
                if (flagged[y][x])
                    sys->text(px + 8, py + 6, "F", sys->rgb(255, 210, 80), 0xFFFFFFFF);
            } else {
                sys->fill(px + 1, py + 1, CELL - 2, CELL - 2,
                          mines[y][x] ? sys->rgb(190, 70, 60) : sys->rgb(200, 206, 216));
                if (mines[y][x])
                    sys->text(px + 8, py + 6, "*", sys->rgb(30, 30, 30), 0xFFFFFFFF);
                else {
                    int n = around(x, y);
                    if (n) {
                        char d[2] = { (char)('0' + n), 0 };
                        sys->text(px + 8, py + 6, d,
                                  sys->rgb(ncol[n][0], ncol[n][1], ncol[n][2]), 0xFFFFFFFF);
                    }
                }
            }
        }
    /* cursor */
    if (!dead && !won)
        sys->rect(cx * CELL, cy * CELL, CELL, CELL, sys->rgb(255, 230, 120));
    char msg[64];
    int flags = 0;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) flags += flagged[y][x];
    sys->format(msg, sizeof(msg), KV_T(sys, "Mines left: %d", "Осталось мин: %d"), MINES - flags);
    if (dead) sys->format(msg, sizeof(msg), KV_T(sys, "Boom! R to restart", "Бум! R - заново"));
    if (won)  sys->format(msg, sizeof(msg), KV_T(sys, "You won! R to restart", "Победа! R - заново"));
    sys->text(4, H - 22, msg, sys->rgb(226, 232, 240), 0xFFFFFFFF);
    sys->status(KV_T(sys, "Click/Enter open, F flag, arrows move, R restart",
                     "Клик/Enter открыть, F флаг, стрелки курсор, R заново"));
}

static kv_app_t me = {
    .title = "Mines",
    .width = W, .height = H,
    .on_open = on_open, .on_draw = on_draw,
    .on_key = on_key, .on_click = on_click,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    me.title = KV_T(sys, "Mines", "Сапёр");
    return &me;
}
