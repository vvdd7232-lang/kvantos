/* Calculator - integer arithmetic with parentheses-free chains
   KAPP_NAME "Calculator"
   Click the buttons or type: digits, + - * / %, Enter = '=',
   Backspace deletes a digit, Esc or C clears. */
#include "kvapp.h"

static const kv_api_t *sys;

/* 4 columns x 5 rows of buttons under the display */
#define COLS 4
#define ROWS 5
static const char *labels[ROWS][COLS] = {
    {"C", "<", "%", "/"},
    {"7", "8", "9", "*"},
    {"4", "5", "6", "-"},
    {"1", "2", "3", "+"},
    {"+/-", "0", "=", "="},
};

static int acc, entry, op, has_entry, err;

#define BTN_X0 10
#define BTN_Y0 64
#define BTN_W  68
#define BTN_H  44
#define BTN_GX 8
#define BTN_GY 8

static kv_u32 c_bg, c_disp, c_btn, c_btn_op, c_btn_eq, c_text, c_dark;

static void recolor(void) {
    c_bg    = sys->rgb(30, 34, 44);
    c_disp  = sys->rgb(20, 24, 32);
    c_btn   = sys->rgb(58, 64, 78);
    c_btn_op= sys->rgb(46, 84, 128);
    c_btn_eq= sys->rgb(46, 128, 84);
    c_text  = sys->rgb(232, 236, 244);
    c_dark  = sys->rgb(140, 148, 162);
}

static void reset_all(void) {
    acc = 0; entry = 0; op = 0; has_entry = 0; err = 0;
}

static int apply(int a, int b, char o) {
    switch (o) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': if (b == 0) { err = 1; return 0; } return a / b;
        case '%': if (b == 0) { err = 1; return 0; } return a % b;
    }
    return b;
}

static void press_op(char o) {
    if (err) return;
    if (op && has_entry) acc = apply(acc, entry, (char)op);
    else if (has_entry || !op) acc = entry;
    if (err) return;
    op = o;
    entry = 0;
    has_entry = 0;
}

static void press_eq(void) {
    if (err || !op) return;
    acc = apply(acc, has_entry ? entry : acc, (char)op);
    op = 0;
    entry = acc;
    has_entry = 1;
}

static void press_digit(int d) {
    if (err) return;
    if (entry > 90000000 || entry < -90000000) return;   /* keep it in range */
    entry = entry * 10 + d;
    has_entry = 1;
}

static void press(char *label) {
    char c = label[0];
    if (c >= '0' && c <= '9' && !label[1]) press_digit(c - '0');
    else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') press_op(c);
    else if (c == '=' && !label[1]) press_eq();
    else if (c == 'C') reset_all();
    else if (c == '<') { entry /= 10; }                    /* backspace */
    else if (label[0] == '+' && label[1] == '-') entry = -entry;
}

static void btn_rect(int r, int col, kv_i32 *x, kv_i32 *y) {
    *x = BTN_X0 + col * (BTN_W + BTN_GX);
    *y = BTN_Y0 + r * (BTN_H + BTN_GY);
}

static void on_draw(void) {
    kv_i32 w = sys->width();
    sys->clear(c_bg);

    /* the display */
    sys->fill(10, 10, w - 20, 44, c_disp);
    sys->rect(10, 10, w - 20, 44, c_dark);
    char buf[32];
    if (err) sys->str_copy(buf, KV_T(sys, "Error (div by 0)", "Ошибка (деление на 0)"), sizeof(buf));
    else sys->format(buf, sizeof(buf), "%d", has_entry || !op ? entry : acc);
    sys->text(w - 18 - sys->text_width(buf), 26, buf, c_text, c_disp);
    if (op && !err) {
        char ob[4] = {(char)op, 0, 0, 0};
        sys->text(18, 26, ob, c_dark, c_disp);
    }

    /* the buttons */
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            if (r == ROWS - 1 && col == 3) continue;       /* wide '=' below */
            kv_i32 x, y;
            btn_rect(r, col, &x, &y);
            const char *lab = labels[r][col];
            kv_u32 fill = c_btn;
            if (lab[0] == '/' || lab[0] == '*' || lab[0] == '-' || lab[0] == '+' || lab[0] == '%') fill = c_btn_op;
            if (lab[0] == 'C' || lab[0] == '<') fill = sys->rgb(96, 58, 58);
            sys->fill(x, y, BTN_W, BTN_H, fill);
            sys->rect(x, y, BTN_W, BTN_H, sys->rgb(16, 18, 24));
            sys->text(x + (BTN_W - sys->text_width(lab)) / 2, y + (BTN_H - KV_CHAR_H) / 2,
                      lab, c_text, fill);
        }
    }
    /* the wide equals key spans the last two cells */
    kv_i32 x, y;
    btn_rect(ROWS - 1, 2, &x, &y);
    kv_i32 wide = BTN_W * 2 + BTN_GX;
    sys->fill(x, y, wide, BTN_H, c_btn_eq);
    sys->rect(x, y, wide, BTN_H, sys->rgb(16, 18, 24));
    sys->text(x + (wide - sys->text_width("=")) / 2, y + (BTN_H - KV_CHAR_H) / 2,
              "=", c_text, c_btn_eq);

    sys->status(KV_T(sys, "Keys work too: 0-9, + - * / %, Enter = '=', Esc clears",
                     "Клавиши тоже работают: 0-9, + - * / %, Enter = '=', Esc - сброс"));
}

static void on_click(kv_i32 x, kv_i32 y, kv_i32 button) {
    (void)button;
    /* the wide equals key */
    kv_i32 ex, ey;
    btn_rect(ROWS - 1, 2, &ex, &ey);
    if (x >= ex && x < ex + BTN_W * 2 + BTN_GX && y >= ey && y < ey + BTN_H) {
        press_eq();
        return;
    }
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            if (r == ROWS - 1 && col == 3) continue;
            kv_i32 bx, by;
            btn_rect(r, col, &bx, &by);
            if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) {
                char lab[4];
                sys->str_copy(lab, labels[r][col], sizeof(lab));
                press(lab);
                return;
            }
        }
    }
}

static void on_key(kv_i32 k) {
    if (k >= '0' && k <= '9') press_digit(k - '0');
    else if (k == '+' || k == '-' || k == '*' || k == '/' || k == '%') press_op((char)k);
    else if (k == KV_KEY_ENTER || k == '=') press_eq();
    else if (k == KV_KEY_BKSP) entry /= 10;
    else if (k == KV_KEY_ESC || k == 'C' || k == 'c') reset_all();
}

static kv_app_t me = {
    .title = "Calculator",
    .width = BTN_X0 * 2 + COLS * BTN_W + (COLS - 1) * BTN_GX,
    .height = BTN_Y0 + ROWS * BTN_H + (ROWS - 1) * BTN_GY + 12,
    .on_open = 0, .on_draw = on_draw, .on_key = on_key, .on_click = on_click,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    recolor();
    reset_all();
    me.title = KV_T(sys, "Calculator", "Калькулятор");
    return &me;
}
