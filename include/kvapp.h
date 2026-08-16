/* ============================================================
 *  KvantOS - application interface (KvApp ABI), version 2
 *
 *  This file is the only contract between the kernel and a program.
 *  Both the kernel and every application include it, so it holds
 *  nothing but definitions: no code, no dependency on libc.
 *
 *  An application is a flat .kapp binary that the kernel loads at a
 *  fixed address before calling one single function, kapp_main().
 *  That function returns a description of the application together
 *  with a set of event handlers - the shell does the rest.
 *
 *  The program runs in the kernel ring (ring 0) and reaches the
 *  system through the kv_api_t function table, a pointer to which it
 *  receives at start-up. Kernel functions are never called by name:
 *  this way an application does not depend on the addresses inside
 *  one particular kernel build.
 * ============================================================ */
#ifndef _KVANT_KVAPP_H
#define _KVANT_KVAPP_H

/* Private types: this header must compile without stdint.h too */
typedef unsigned char      kv_u8;
typedef unsigned short     kv_u16;
typedef unsigned int       kv_u32;
typedef signed   int       kv_i32;

/* Interface version. The kernel refuses to start an application
   built for an incompatible version and says so explicitly. */
#define KV_API_VERSION   2

/* The .kapp file format */
#define KAPP_MAGIC0 'K'
#define KAPP_MAGIC1 'A'
#define KAPP_MAGIC2 'P'
#define KAPP_MAGIC3 'P'
#define KAPP_FORMAT_VERSION 1

/* The address at which the kernel unpacks an application.
   The 14-16 MiB region is reserved in the memory manager. */
#define KAPP_LOAD_BASE   0x00E00000u
#define KAPP_MAX_SIZE    0x00200000u      /* 2 MiB for code, data and bss */

/* Header flags */
#define KAPP_FLAG_WINDOW  0x0001u         /* windowed application */

/* The .kapp header is exactly 64 bytes, the memory image follows */
typedef struct {
    char   magic[4];        /* 'K','A','P','P'                        */
    kv_u16 version;         /* file format version                    */
    kv_u16 header_size;     /* size of this header (64)               */
    kv_u32 api_version;     /* ABI version it was built against       */
    kv_u32 flags;           /* KAPP_FLAG_*                            */
    kv_u32 load_base;       /* load address, must be KAPP_LOAD_BASE   */
    kv_u32 entry;           /* absolute address of kapp_main          */
    kv_u32 code_size;       /* how many bytes to read from the file   */
    kv_u32 bss_size;        /* how many bytes to zero afterwards      */
    char   name[32];        /* name shown on the desktop              */
} kapp_header_t;

/* ---------- keyboard events ----------
   Ordinary characters arrive as ASCII/CP866 codes. Special keys use
   values above 255 that match the kernel's internal codes. */
#define KV_KEY_UP     0x100
#define KV_KEY_DOWN   0x101
#define KV_KEY_LEFT   0x102
#define KV_KEY_RIGHT  0x103
#define KV_KEY_ENTER  '\n'
#define KV_KEY_ESC    27
#define KV_KEY_BKSP   8
#define KV_KEY_TAB    9

/* Cell size of the built-in font - needed for layout */
#define KV_CHAR_W 8
#define KV_CHAR_H 16

/* ============================================================
 *  System function table
 *
 *  The application receives a pointer to it in kapp_main() and must
 *  keep it. All drawing coordinates are measured from the top left
 *  corner of the window client area; anything outside is clipped by
 *  the kernel, so corrupting another window is impossible.
 * ============================================================ */
typedef struct kv_api {
    kv_u32 api_version;          /* matches KV_API_VERSION */

    /* ---- canvas size ---- */
    kv_i32 (*width)(void);       /* client area width, pixels  */
    kv_i32 (*height)(void);      /* client area height         */

    /* ---- drawing ---- */
    kv_u32 (*rgb)(kv_u8 r, kv_u8 g, kv_u8 b);
    void   (*clear)(kv_u32 color);
    void   (*pixel)(kv_i32 x, kv_i32 y, kv_u32 color);
    void   (*fill)(kv_i32 x, kv_i32 y, kv_i32 w, kv_i32 h, kv_u32 color);
    void   (*rect)(kv_i32 x, kv_i32 y, kv_i32 w, kv_i32 h, kv_u32 color);
    void   (*line)(kv_i32 x0, kv_i32 y0, kv_i32 x1, kv_i32 y1, kv_u32 color);
    /* Text is UTF-8: Cyrillic is fine. bg = 0xFFFFFFFF means a transparent background */
    void   (*text)(kv_i32 x, kv_i32 y, const char *s, kv_u32 fg, kv_u32 bg);
    kv_i32 (*text_width)(const char *s);   /* string width in pixels */

    /* ---- window status line ---- */
    void   (*status)(const char *s);

    /* ---- memory ---- */
    void  *(*alloc)(kv_u32 size);
    void   (*release)(void *p);

    /* ---- time and sound ---- */
    kv_u32 (*ticks)(void);       /* system timer ticks since boot       */
    kv_u32 (*hz)(void);          /* timer frequency, ticks per second   */
    kv_u32 (*seconds)(void);     /* seconds since boot                  */
    void   (*clock)(kv_i32 *h, kv_i32 *m, kv_i32 *s);   /* real-time clock */
    void   (*beep)(kv_u32 freq, kv_u32 ms);

    /* ---- files on the disk (KvFS) ----
       Return a byte count or a negative error code. */
    kv_i32 (*file_read)(const char *name, void *buf, kv_u32 max);
    kv_i32 (*file_write)(const char *name, const void *buf, kv_u32 size);
    kv_i32 (*file_delete)(const char *name);
    kv_i32 (*file_list)(kv_i32 index, char *name40, kv_u32 *size);
    kv_i32 (*file_exists)(const char *name);

    /* ---- strings and memory: an application has no libc ---- */
    void  *(*mem_set)(void *d, int c, kv_u32 n);
    void  *(*mem_copy)(void *d, const void *s, kv_u32 n);
    kv_u32 (*str_len)(const char *s);
    kv_i32 (*str_cmp)(const char *a, const char *b);
    void   (*str_copy)(char *d, const char *s, kv_u32 max);
    /* Formatting into a buffer: supports %d %u %x %s %c %% */
    void   (*format)(char *buf, kv_u32 size, const char *fmt, ...);

    /* ---- miscellaneous ---- */
    kv_u32 (*random)(void);      /* pseudo-random number */
    void   (*log)(const char *s);/* a line into the system log (terminal) */

    /* Interface language: 0 - English, 1 - Russian. An application can
       label its buttons in the same language as the system.
       Added in ABI 2. */
    kv_u32 (*lang)(void);
} kv_api_t;

/* Picks a string according to the current system language. Requires the
   application to hold a kv_api_t pointer (see the samples in sdk/apps). */
#define KV_T(api, en, ru)  ((api)->lang() ? (ru) : (en))

/* ============================================================
 *  Application description
 *
 *  kapp_main() fills this structure in and returns a pointer to it.
 *  The structure must live for as long as the program runs - the
 *  simplest way is to declare it static.
 *
 *  Any handler may be left null when it is not needed.
 * ============================================================ */
typedef struct kv_app {
    const char *title;           /* window title                       */
    kv_i32      width, height;   /* desired client area size           */

    void (*on_open)(void);                       /* the window was opened  */
    void (*on_draw)(void);                       /* draw a frame           */
    void (*on_key)(kv_i32 key);                  /* a key was pressed      */
    void (*on_click)(kv_i32 x, kv_i32 y, kv_i32 button);  /* mouse click */
    void (*on_tick)(void);                       /* once per frame, before draw */
    void (*on_close)(void);                      /* the window is closing  */
} kv_app_t;

/* The entry point every application must define */
typedef kv_app_t *(*kapp_entry_t)(const kv_api_t *api);

#endif /* _KVANT_KVAPP_H */
