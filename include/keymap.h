#ifndef KV_KEYMAP_H
#define KV_KEYMAP_H
/* One queued event contains legacy physical input AND optional Unicode text.
 * Unicode is never encoded into the byte-sized KEY_* command namespace. */
#define KEY_UP     0x81
#define KEY_DOWN   0x82
#define KEY_LEFT   0x83
#define KEY_RIGHT  0x84
#define KEY_HOME   0x85
#define KEY_END    0x86
#define KEY_PGUP   0x87
#define KEY_PGDN   0x88
#define KEY_DEL    0x89
#define KEY_INS    0x8A
#define KEY_F1     0x90
#define KEY_F2     0x91
#define KEY_F3     0x92
#define KEY_F4     0x93
#define KEY_F5     0x94
#define KEY_F6     0x95
#define KEY_F7     0x96
#define KEY_F8     0x97
#define KEY_F9     0x98
#define KEY_F10    0x99
#define KEY_LAYOUT 0x100
#define KMOD_SHIFT 1u
#define KMOD_CTRL  2u
#define KMOD_ALT   4u

/* physical is the EN key before Ctrl folding: Ctrl+H is not Ctrl+Backspace. */
typedef struct { int key; unsigned codepoint, modifiers, physical; } kbd_event_t;
typedef struct {
    unsigned pressed;
    int layout, caps, extended, skip, combo;
} keymap_t;
void keymap_init(keymap_t *s);
int keymap_feed(keymap_t *s, unsigned scan, kbd_event_t *event);
unsigned keymap_modifiers(const keymap_t *s);
unsigned keymap_utf8(unsigned codepoint, char out[4]);
#endif
