/* ============================================================
 *  Заметки - пример работы с файлами и клавиатурой
 *  KAPP_NAME "Заметки"
 *
 *  Показывает то, чего нет в примере «Часы»:
 *    - ввод текста с клавиатуры;
 *    - сохранение и чтение файла на диске;
 *    - собственные кнопки и обработка щелчков.
 *
 *  Заметка хранится на диске в файле notes.txt, поэтому
 *  переживает перезагрузку.
 * ============================================================ */
#include "kvapp.h"

#define FILE_NAME "notes.txt"
#define MAX_LEN   1024
#define LINE_MAX  46           /* символов в строке при ширине окна 400 */

static const kv_api_t *sys;

static char text[MAX_LEN + 1];
static kv_u32 len = 0;
static int dirty = 0;
static kv_u32 blink_mark = 0;

static kv_u32 col_bg, col_paper, col_text, col_dim, col_btn, col_btn_text, col_accent;

static void recolor(void) {
    col_bg       = sys->rgb(238, 241, 246);
    col_paper    = sys->rgb(255, 255, 255);
    col_text     = sys->rgb(28, 34, 46);
    col_dim      = sys->rgb(130, 140, 158);
    col_btn      = sys->rgb(38, 92, 168);
    col_btn_text = sys->rgb(255, 255, 255);
    col_accent   = sys->rgb(46, 130, 80);
}

static void load(void) {
    kv_i32 got = sys->file_read(FILE_NAME, text, MAX_LEN);
    if (got > 0) {
        len = (kv_u32)got;
        text[len] = 0;
        sys->status("Заметка загружена с диска");
    } else {
        len = 0;
        text[0] = 0;
        sys->status("Новая заметка. Пишите и нажмите Ctrl+S");
    }
    dirty = 0;
}

static void save(void) {
    kv_i32 rc = sys->file_write(FILE_NAME, text, len);
    if (rc == 0) {
        dirty = 0;
        sys->status("Сохранено на диск");
        sys->beep(1200, 40);
    } else {
        sys->status("Не удалось сохранить: нет диска или места");
    }
}

/* Кнопки: координаты считаем один раз, чтобы рисование и щелчки
   пользовались одними и теми же числами. */
static void button_box(int index, kv_i32 *x, kv_i32 *y, kv_i32 *w, kv_i32 *h) {
    *w = 96; *h = 26;
    *x = 12 + index * (*w + 10);
    *y = 10;
}

static void draw_button(int index, const char *label, kv_u32 color) {
    kv_i32 x, y, w, h;
    button_box(index, &x, &y, &w, &h);
    sys->fill(x, y, w, h, color);
    kv_i32 tw = sys->text_width(label);
    sys->text(x + (w - tw) / 2, y + 5, label, col_btn_text, 0xFFFFFFFF);
}

static void on_draw(void) {
    kv_i32 w = sys->width(), h = sys->height();
    sys->clear(col_bg);

    draw_button(0, "Сохранить", dirty ? col_btn : col_dim);
    draw_button(1, "Загрузить", col_btn);
    draw_button(2, "Очистить",  col_accent);

    /* лист с текстом */
    kv_i32 py = 46;
    sys->fill(10, py, w - 20, h - py - 10, col_paper);
    sys->rect(10, py, w - 20, h - py - 10, sys->rgb(196, 204, 218));

    /* Перенос по ширине: считаем сами, ядро этого не делает */
    kv_i32 tx = 18, ty = py + 8;
    kv_u32 col = 0;
    for (kv_u32 i = 0; i < len; i++) {
        char ch[2];
        ch[0] = text[i];
        ch[1] = 0;
        if (text[i] == '\n' || col >= LINE_MAX) {
            ty += KV_CHAR_H;
            col = 0;
            tx = 18;
            if (text[i] == '\n') continue;
        }
        if (ty + KV_CHAR_H > h - 14) break;
        sys->text(tx, ty, ch, col_text, 0xFFFFFFFF);
        tx += KV_CHAR_W;
        col++;
    }

    /* мигающий курсор: полсекунды виден, полсекунды нет */
    kv_u32 hz = sys->hz();
    if (hz && ((sys->ticks() - blink_mark) / (hz / 2)) % 2 == 0)
        if (ty + KV_CHAR_H <= h - 14)
            sys->fill(tx, ty, 2, KV_CHAR_H, col_text);

    /* счётчик символов */
    char info[48];
    sys->format(info, sizeof(info), "%u из %u символов%s",
                len, (kv_u32)MAX_LEN, dirty ? "  *" : "");
    sys->text(12, h - 8 - KV_CHAR_H + 2, info, col_dim, 0xFFFFFFFF);
}

static void on_key(kv_i32 key) {
    if (key == 19) { save(); return; }          /* Ctrl+S */
    if (key == 12) { load(); return; }          /* Ctrl+L */

    if (key == KV_KEY_BKSP) {
        if (len > 0) { len--; text[len] = 0; dirty = 1; }
        return;
    }
    if (key == KV_KEY_ENTER) {
        if (len < MAX_LEN) { text[len++] = '\n'; text[len] = 0; dirty = 1; }
        return;
    }
    /* Обычные символы. Управляющие коды и спецклавиши пропускаем. */
    if (key >= 32 && key < 256 && len < MAX_LEN) {
        text[len++] = (char)key;
        text[len] = 0;
        dirty = 1;
    }
}

static void on_click(kv_i32 mx, kv_i32 my, kv_i32 button) {
    (void)button;
    for (int i = 0; i < 3; i++) {
        kv_i32 x, y, w, h;
        button_box(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) {
            if (i == 0) save();
            else if (i == 1) load();
            else { len = 0; text[0] = 0; dirty = 1; sys->status("Очищено"); }
            return;
        }
    }
}

static void on_open(void) {
    blink_mark = sys->ticks();
    load();
    sys->log("Заметки: запущены");
}

static kv_app_t me = {
    .title  = "Заметки",
    .width  = 400,
    .height = 300,
    .on_open  = on_open,
    .on_draw  = on_draw,
    .on_key   = on_key,
    .on_click = on_click,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    recolor();
    return &me;
}
