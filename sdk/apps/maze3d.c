/* Maze 3D - a small integer-only ray-cast escape game
   KAPP_NAME "Maze 3D"
   WASD move/strafe, arrows turn, space restarts. Find the golden exit. */
#include "kvapp.h"

static const kv_api_t *sys;

#define MAP_W 12
#define MAP_H 12
#define VIEW_W 480
#define VIEW_H 280
#define MOVE_Q8 64
#define CAMERA_PLANE_Q10 590
#define FOCAL_PIXELS 230

/* 0 = floor, 1 = stone wall, 2 = the exit. */
static const kv_u8 maze[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,1,0,1,0,1},
    {1,0,0,0,1,0,0,0,0,1,0,1},
    {1,1,1,0,1,0,1,1,0,1,0,1},
    {1,0,0,0,1,0,0,1,0,0,0,1},
    {1,0,1,1,1,0,0,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,0,1,1,1,0,1,0,1},
    {1,0,0,1,0,0,0,0,0,1,0,1},
    {1,0,0,0,0,0,0,0,0,2,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1},
};

/* One quarter-wave of sin(angle), Q10; one full circle is 64 steps. */
static const kv_i32 sin_quarter[17] = {
    0, 100, 200, 297, 392, 483, 569, 650, 724,
    792, 851, 903, 946, 980, 1004, 1019, 1024
};

static kv_i32 player_x, player_y; /* map coordinates in Q8 */
static int facing;                /* 64 compass steps per revolution */
static int won;
static kv_u32 pulse_ms;

static kv_i32 sine(int angle) {
    angle &= 63;
    int quadrant = angle >> 4;
    int offset = angle & 15;
    if (quadrant == 0) return sin_quarter[offset];
    if (quadrant == 1) return sin_quarter[16 - offset];
    if (quadrant == 2) return -sin_quarter[offset];
    return -sin_quarter[16 - offset];
}

static kv_i32 cosine(int angle) {
    return sine(angle + 16);
}

static int solid_at(kv_i32 x, kv_i32 y) {
    int mx = x >> 8, my = y >> 8;
    if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) return 1;
    return maze[my][mx] == 1;
}

static void check_exit(void) {
    int mx = player_x >> 8, my = player_y >> 8;
    if (mx >= 0 && my >= 0 && mx < MAP_W && my < MAP_H && maze[my][mx] == 2)
        won = 1;
}

static void restart(void) {
    player_x = 1 * 256 + 128;
    player_y = 1 * 256 + 128;
    facing = 0; /* east */
    won = 0;
    pulse_ms = 0;
    sys->status(KV_T(sys, "WASD move/strafe, arrows turn, Space restart",
                     "WASD — ходьба/шаг, стрелки — поворот, пробел — заново"));
}

static void move_player(int angle, int direction) {
    kv_i32 dx = cosine(angle) * MOVE_Q8 * direction / 1024;
    kv_i32 dy = sine(angle) * MOVE_Q8 * direction / 1024;
    kv_i32 nx = player_x + dx, ny = player_y + dy;

    /* Separate axis checks let the player slide along a wall. */
    if (!solid_at(nx, player_y)) player_x = nx;
    if (!solid_at(player_x, ny)) player_y = ny;
    check_exit();
}

static int cell_at(int x, int y) {
    if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) return 1;
    return maze[y][x];
}

static kv_u32 shade(kv_u8 r, kv_u8 g, kv_u8 b, int light) {
    if (light < 36) light = 36;
    if (light > 255) light = 255;
    return sys->rgb((kv_u8)((kv_u32)r * (kv_u32)light / 255u),
                    (kv_u8)((kv_u32)g * (kv_u32)light / 255u),
                    (kv_u8)((kv_u32)b * (kv_u32)light / 255u));
}

static void cast_column(int x) {
    kv_i32 dir_x = cosine(facing), dir_y = sine(facing);
    kv_i32 plane_x = -dir_y * CAMERA_PLANE_Q10 / 1024;
    kv_i32 plane_y =  dir_x * CAMERA_PLANE_Q10 / 1024;
    kv_i32 camera_x = ((2 * x + 1) * 1024 / VIEW_W) - 1024;
    kv_i32 ray_x = dir_x + plane_x * camera_x / 1024;
    kv_i32 ray_y = dir_y + plane_y * camera_x / 1024;

    int map_x = player_x >> 8;
    int map_y = player_y >> 8;
    int step_x, step_y, side = 0;
    kv_i32 abs_x = ray_x < 0 ? -ray_x : ray_x;
    kv_i32 abs_y = ray_y < 0 ? -ray_y : ray_y;
    kv_i32 delta_x = abs_x ? (262144 / abs_x) : 0x3fffffff;
    kv_i32 delta_y = abs_y ? (262144 / abs_y) : 0x3fffffff;
    kv_i32 side_x, side_y;

    if (ray_x < 0) {
        step_x = -1;
        side_x = (player_x - map_x * 256) * 1024 / (abs_x ? abs_x : 1);
    } else {
        step_x = 1;
        side_x = ((map_x + 1) * 256 - player_x) * 1024 / (abs_x ? abs_x : 1);
    }
    if (ray_y < 0) {
        step_y = -1;
        side_y = (player_y - map_y * 256) * 1024 / (abs_y ? abs_y : 1);
    } else {
        step_y = 1;
        side_y = ((map_y + 1) * 256 - player_y) * 1024 / (abs_y ? abs_y : 1);
    }

    int wall = 1;
    for (int step = 0; step < 48; step++) {
        if (side_x < side_y) {
            side_x += delta_x;
            map_x += step_x;
            side = 0;
        } else {
            side_y += delta_y;
            map_y += step_y;
            side = 1;
        }
        wall = cell_at(map_x, map_y);
        if (wall != 0) break;
    }

    kv_i32 distance = side ? side_y - delta_y : side_x - delta_x;
    if (distance < 1) distance = 1;
    kv_i32 wall_h = (FOCAL_PIXELS * 256) / distance;
    if (wall_h > VIEW_H) wall_h = VIEW_H;
    if (wall_h < 1) wall_h = 1;
    kv_i32 top = (VIEW_H - wall_h) / 2;

    int depth = distance >> 8;
    int light = 224 - depth * 15;
    if (side) light = light * 3 / 4;
    kv_u32 colour;
    if (wall == 2) {
        int glow = pulse_ms < 600 ? 160 + (int)pulse_ms * 80 / 600
                                  : 240 - ((int)pulse_ms - 600) * 80 / 600;
        colour = shade(255, 194, 54, light * glow / 220);
    } else if (side) {
        colour = shade(82, 132, 184, light);
    } else {
        colour = shade(174, 92, 62, light);
    }
    sys->fill(x, top, 2, wall_h, colour);
}

static void draw_minimap(void) {
    const int cell = 5;
    const int ox = VIEW_W - MAP_W * cell - 10;
    const int oy = 8;
    sys->fill(ox - 4, oy - 4, MAP_W * cell + 8, MAP_H * cell + 8,
              sys->rgb(8, 12, 20));
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            int v = maze[y][x];
            if (v == 1)
                sys->fill(ox + x * cell, oy + y * cell, cell - 1, cell - 1,
                          sys->rgb(92, 105, 124));
            else if (v == 2)
                sys->fill(ox + x * cell, oy + y * cell, cell - 1, cell - 1,
                          sys->rgb(255, 190, 48));
        }
    }
    int px = ox + (player_x >> 8) * cell + cell / 2;
    int py = oy + (player_y >> 8) * cell + cell / 2;
    sys->fill(px - 2, py - 2, 4, 4, sys->rgb(100, 245, 150));
    sys->line(px, py, px + cosine(facing) * 9 / 1024,
              py + sine(facing) * 9 / 1024, sys->rgb(100, 245, 150));
}

static void on_tick(void) {
    pulse_ms += sys->delta_ms();
    if (pulse_ms >= 1200) pulse_ms %= 1200;
}

static void on_draw(void) {
    sys->clear(sys->rgb(10, 15, 24));
    sys->fill(0, 0, VIEW_W, VIEW_H / 2, sys->rgb(28, 46, 70));
    sys->fill(0, VIEW_H / 2, VIEW_W, VIEW_H / 2, sys->rgb(42, 36, 32));
    sys->line(0, VIEW_H / 2, VIEW_W, VIEW_H / 2, sys->rgb(82, 75, 68));

    for (int x = 0; x < VIEW_W; x += 2) cast_column(x);
    draw_minimap();

    /* A compact crosshair anchors the view and the message marks success. */
    int cx = VIEW_W / 2, cy = VIEW_H / 2;
    sys->fill(cx - 5, cy, 11, 1, sys->rgb(230, 235, 240));
    sys->fill(cx, cy - 5, 1, 11, sys->rgb(230, 235, 240));
    sys->text(8, VIEW_H + 7,
              KV_T(sys, "Find the golden exit", "Найдите золотой выход"),
              sys->rgb(214, 222, 234), 0xFFFFFFFF);
    if (won) {
        const char *msg = KV_T(sys, "ESCAPE COMPLETE! Space to play again",
                               "ВЫХОД НАЙДЕН! Пробел — ещё раз");
        sys->fill(VIEW_W / 2 - 145, VIEW_H / 2 - 24, 290, 48,
                  sys->rgb(22, 40, 34));
        sys->text(VIEW_W / 2 - sys->text_width(msg) / 2, VIEW_H / 2 - 7,
                  msg, sys->rgb(130, 245, 165), 0xFFFFFFFF);
    }
    sys->status(KV_T(sys, "W/S move, A/D strafe, arrows turn, Space restart",
                     "W/S — ходьба, A/D — шаг вбок, стрелки — поворот, пробел — заново"));
}

static void on_key(kv_i32 key) {
    if (key == ' ' || key == 'R' || key == 'r') { restart(); return; }
    if (key == KV_KEY_LEFT) { facing = (facing + 63) & 63; return; }
    if (key == KV_KEY_RIGHT) { facing = (facing + 1) & 63; return; }
    if (key == KV_KEY_UP || key == 'W' || key == 'w') move_player(facing, 1);
    else if (key == KV_KEY_DOWN || key == 'S' || key == 's') move_player(facing, -1);
    else if (key == 'A' || key == 'a') move_player(facing - 16, 1);
    else if (key == 'D' || key == 'd') move_player(facing + 16, 1);
}

static kv_app_t me = {
    .title = "Maze 3D",
    .width = VIEW_W,
    .height = VIEW_H + 34,
    .on_open = restart,
    .on_draw = on_draw,
    .on_key = on_key,
    .on_tick = on_tick,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    me.title = KV_T(sys, "Maze 3D", "Лабиринт 3D");
    return &me;
}
