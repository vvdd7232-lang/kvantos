#ifndef KV_TEXTEDIT_H
#define KV_TEXTEDIT_H
/* Bounded text model shared by both GUI editors; no framebuffer or filesystem. */
#define ED_WARN_LINES 128
#define ED_COLS 120
#define ED_CAP 65536 /* byte budget, not a line-count ceiling */
typedef struct {
    char data[ED_CAP]; /* NUL-separated lines, one final terminator */
    unsigned short start[ED_CAP]; /* one offset per possible newline */
    int lines, cy, cx, top, left, dirty;
    int ay, ax, selecting;
    unsigned used;
    int width; /* cached longest line in display columns */
} textdoc_t;
typedef struct { textdoc_t doc; int valid; } textundo_t;
const char *text_line(const textdoc_t *d, int row);
void text_copy(textdoc_t *dst, const textdoc_t *src);
void text_reset(textdoc_t *d);
int text_set(textdoc_t *d, const char *s, unsigned n);
unsigned text_export(const textdoc_t *d, char *s);
unsigned text_offset(const textdoc_t *d, int row, int col);
void text_position(textdoc_t *d, unsigned offset);
int text_selected(const textdoc_t *d);
void text_range(const textdoc_t *d, unsigned *start, unsigned *end);
int text_replace(textdoc_t *d, textundo_t *u, const char *s, unsigned n);
int text_delete(textdoc_t *d, textundo_t *u, int backwards);
int text_undo(textdoc_t *d, textundo_t *u);
int text_next(const char *s, int col);
int text_prev(const char *s, int col);
int text_column(const char *s, int byte);
int text_byte(const char *s, int column);
#endif
