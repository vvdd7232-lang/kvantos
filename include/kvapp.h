/* ============================================================
 *  KvantOS - интерфейс приложений (KvApp ABI), версия 1
 *
 *  Этот файл - единственный договор между ядром и программой.
 *  Его включает и ядро, и любое приложение, поэтому здесь нет
 *  ничего, кроме определений: ни кода, ни зависимостей от libc.
 *
 *  Приложение - это плоский двоичный файл .kapp, который ядро
 *  загружает по фиксированному адресу и вызывает одну-единственную
 *  функцию kapp_main(). Она возвращает описание приложения с
 *  набором обработчиков событий - дальше всё делает оболочка.
 *
 *  Программа работает в кольце ядра (ring 0) и обращается к системе
 *  через таблицу функций kv_api_t, указатель на которую получает
 *  при запуске. Прямых вызовов функций ядра по имени нет: так
 *  приложение не зависит от адресов внутри конкретной сборки ядра.
 * ============================================================ */
#ifndef _KVANT_KVAPP_H
#define _KVANT_KVAPP_H

/* Собственные типы: заголовок обязан собираться и без stdint.h */
typedef unsigned char      kv_u8;
typedef unsigned short     kv_u16;
typedef unsigned int       kv_u32;
typedef signed   int       kv_i32;

/* Версия интерфейса. Ядро откажется запускать приложение,
   собранное для несовместимой версии, и honestly скажет об этом. */
#define KV_API_VERSION   1

/* Формат файла .kapp */
#define KAPP_MAGIC0 'K'
#define KAPP_MAGIC1 'A'
#define KAPP_MAGIC2 'P'
#define KAPP_MAGIC3 'P'
#define KAPP_FORMAT_VERSION 1

/* Адрес, по которому ядро разворачивает приложение.
   Область 14-16 МиБ зарезервирована в диспетчере памяти. */
#define KAPP_LOAD_BASE   0x00E00000u
#define KAPP_MAX_SIZE    0x00200000u      /* 2 МиБ на код, данные и bss */

/* Флаги в заголовке */
#define KAPP_FLAG_WINDOW  0x0001u         /* оконное приложение */

/* Заголовок файла .kapp - ровно 64 байта, дальше идёт образ памяти */
typedef struct {
    char   magic[4];        /* 'K','A','P','P'                        */
    kv_u16 version;         /* версия формата файла                   */
    kv_u16 header_size;     /* размер этого заголовка (64)            */
    kv_u32 api_version;     /* версия ABI, под которую собрано        */
    kv_u32 flags;           /* KAPP_FLAG_*                            */
    kv_u32 load_base;       /* адрес загрузки, должен быть KAPP_LOAD_BASE */
    kv_u32 entry;           /* абсолютный адрес функции kapp_main     */
    kv_u32 code_size;       /* сколько байт читать из файла           */
    kv_u32 bss_size;        /* сколько байт обнулить следом           */
    char   name[32];        /* название для рабочего стола            */
} kapp_header_t;

/* ---------- события клавиатуры ----------
   Обычные символы приходят кодом ASCII/CP866. Специальные клавиши -
   значениями выше 255, они совпадают с внутренними кодами ядра. */
#define KV_KEY_UP     0x100
#define KV_KEY_DOWN   0x101
#define KV_KEY_LEFT   0x102
#define KV_KEY_RIGHT  0x103
#define KV_KEY_ENTER  '\n'
#define KV_KEY_ESC    27
#define KV_KEY_BKSP   8
#define KV_KEY_TAB    9

/* Размер знакоместа встроенного шрифта - нужен для вёрстки */
#define KV_CHAR_W 8
#define KV_CHAR_H 16

/* ============================================================
 *  Таблица системных функций
 *
 *  Указатель на неё приложение получает в kapp_main() и обязано
 *  сохранить. Все координаты рисования отсчитываются от левого
 *  верхнего угла клиентской области окна; выход за её границы
 *  обрезается ядром, испортить чужое окно невозможно.
 * ============================================================ */
typedef struct kv_api {
    kv_u32 api_version;          /* совпадает с KV_API_VERSION */

    /* ---- размеры холста ---- */
    kv_i32 (*width)(void);       /* ширина клиентской области, пикселей */
    kv_i32 (*height)(void);      /* высота клиентской области           */

    /* ---- рисование ---- */
    kv_u32 (*rgb)(kv_u8 r, kv_u8 g, kv_u8 b);
    void   (*clear)(kv_u32 color);
    void   (*pixel)(kv_i32 x, kv_i32 y, kv_u32 color);
    void   (*fill)(kv_i32 x, kv_i32 y, kv_i32 w, kv_i32 h, kv_u32 color);
    void   (*rect)(kv_i32 x, kv_i32 y, kv_i32 w, kv_i32 h, kv_u32 color);
    void   (*line)(kv_i32 x0, kv_i32 y0, kv_i32 x1, kv_i32 y1, kv_u32 color);
    /* Текст в кодировке UTF-8: кириллица допустима. bg = 0xFFFFFFFF - прозрачный фон */
    void   (*text)(kv_i32 x, kv_i32 y, const char *s, kv_u32 fg, kv_u32 bg);
    kv_i32 (*text_width)(const char *s);   /* ширина строки в пикселях */

    /* ---- строка состояния окна ---- */
    void   (*status)(const char *s);

    /* ---- память ---- */
    void  *(*alloc)(kv_u32 size);
    void   (*release)(void *p);

    /* ---- время и звук ---- */
    kv_u32 (*ticks)(void);       /* тиков системного таймера с загрузки */
    kv_u32 (*hz)(void);          /* частота таймера, тиков в секунду    */
    kv_u32 (*seconds)(void);     /* секунд с загрузки                   */
    void   (*clock)(kv_i32 *h, kv_i32 *m, kv_i32 *s);   /* часы реального времени */
    void   (*beep)(kv_u32 freq, kv_u32 ms);

    /* ---- файлы на диске (KvFS) ----
       Возвращают число байт либо отрицательный код ошибки. */
    kv_i32 (*file_read)(const char *name, void *buf, kv_u32 max);
    kv_i32 (*file_write)(const char *name, const void *buf, kv_u32 size);
    kv_i32 (*file_delete)(const char *name);
    kv_i32 (*file_list)(kv_i32 index, char *name40, kv_u32 *size);
    kv_i32 (*file_exists)(const char *name);

    /* ---- строки и память: у приложения нет libc ---- */
    void  *(*mem_set)(void *d, int c, kv_u32 n);
    void  *(*mem_copy)(void *d, const void *s, kv_u32 n);
    kv_u32 (*str_len)(const char *s);
    kv_i32 (*str_cmp)(const char *a, const char *b);
    void   (*str_copy)(char *d, const char *s, kv_u32 max);
    /* Форматирование в буфер: поддерживает %d %u %x %s %c %% */
    void   (*format)(char *buf, kv_u32 size, const char *fmt, ...);

    /* ---- прочее ---- */
    kv_u32 (*random)(void);      /* псевдослучайное число */
    void   (*log)(const char *s);/* строка в системный журнал (терминал) */
} kv_api_t;

/* ============================================================
 *  Описание приложения
 *
 *  kapp_main() заполняет эту структуру и возвращает указатель
 *  на неё. Структура обязана жить всё время работы программы -
 *  проще всего объявить её статической.
 *
 *  Любой обработчик можно оставить нулевым, если он не нужен.
 * ============================================================ */
typedef struct kv_app {
    const char *title;           /* заголовок окна                     */
    kv_i32      width, height;   /* желаемый размер клиентской области */

    void (*on_open)(void);                       /* окно открыли        */
    void (*on_draw)(void);                       /* нарисовать кадр     */
    void (*on_key)(kv_i32 key);                  /* нажата клавиша      */
    void (*on_click)(kv_i32 x, kv_i32 y, kv_i32 button);  /* щелчок мышью */
    void (*on_tick)(void);                       /* раз в кадр, до draw */
    void (*on_close)(void);                      /* окно закрывают      */
} kv_app_t;

/* Точка входа, которую обязано определить каждое приложение */
typedef kv_app_t *(*kapp_entry_t)(const kv_api_t *api);

#endif /* _KVANT_KVAPP_H */
