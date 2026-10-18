/* PS/2 keyboard, scan code set 1, ring buffer */
#include "kernel.h"

#define BUF_SIZE 256
static volatile kbd_event_t buf[BUF_SIZE];
static volatile u32 kbd_head = 0, kbd_tail = 0;
static keymap_t keymap;
static int mousekeys, mk_speed=1;
static u64 mk_release;
static void kbd_sync_leds(void);   /* defined below */

static void push(kbd_event_t e) {
    u32 n=(kbd_head+1)%BUF_SIZE;
    if(n!=kbd_tail) { buf[kbd_head]=e; kbd_head=n; }
}

static void kbd_cb(registers_t *r) {
    (void)r;

    /* Port 0x60 is read ONLY when the buffer really holds a byte.
       Otherwise a race appeared: the timer poll kbd_poll() grabbed the
       scan code first, and this handler read an already empty port and
       received the same byte again - characters were doubled
       ("gui" -> "ggui"). Reading an empty buffer returns the previous
       value. */
    if (!(inb(0x64) & 0x01)) return;

    u8 sc = inb(0x60);

    /* Replies from the controller itself rather than key presses. The
       0xFA (ACK) answer to an LED command used to fall through into the
       generic parsing as a scan code and broke input. */
    if (sc == 0xFA || sc == 0xFE || sc == 0xEE || sc == 0x00 || sc == 0xFF)
        return;

    /* ---------- MouseKeys ----------
       With NumLock on, the keypad drives the pointer instead of typing
       digits: 8/2/4/6 move it, 7/9/1/3 move it diagonally, 5 clicks,
       0 holds the button down (for dragging), . releases it, + and -
       change the step. Holding Shift moves by a single pixel for
       precise aiming, Ctrl moves in large jumps.

       The keypad digits were not reachable at all before this (they are
       absent from map_lower), so nothing that used to work changes. */
    unsigned mods=keymap_modifiers(&keymap);
    if (mousekeys && !keymap.extended && !keymap.skip && !(sc&0x80)) {
        i32 step = 8;
        if (mods&KMOD_SHIFT) step = 1;            /* precise */
        else if (mods&KMOD_CTRL) step = 48;       /* fast */
        step *= mk_speed;

        switch (sc) {
            case 0x48: mouse_move_rel(0, -step); return;   /* 8 */
            case 0x50: mouse_move_rel(0,  step); return;   /* 2 */
            case 0x4B: mouse_move_rel(-step, 0); return;   /* 4 */
            case 0x4D: mouse_move_rel( step, 0); return;   /* 6 */
            case 0x47: mouse_move_rel(-step, -step); return; /* 7 */
            case 0x49: mouse_move_rel( step, -step); return; /* 9 */
            case 0x4F: mouse_move_rel(-step,  step); return; /* 1 */
            case 0x51: mouse_move_rel( step,  step); return; /* 3 */
            case 0x4C:                                     /* 5 - click */
                mouse_set_button(0, 1);
                mk_release = timer_ticks()+timer_hz()*60/1000+1;
                return;
            case 0x52: mk_release=0; mouse_set_button(0, 1); return;     /* 0 - hold */
            case 0x53: mk_release=0; mouse_set_button(0, 0); return;     /* . - release */
            case 0x4E:                                     /* + faster */
                if (mk_speed < 8) mk_speed++;
                return;
            case 0x4A:                                     /* - slower */
                if (mk_speed > 1) mk_speed--;
                return;
        }
    }

    if(sc==0x45 && !keymap.extended && !keymap.skip) { kbd_mousekeys_set(!mousekeys); return; }
    int caps=keymap.caps;
    kbd_event_t e;
    if(keymap_feed(&keymap,sc,&e)) push(e);
    if(caps!=keymap.caps) kbd_sync_leds();
}

/* Keyboard LED control.
   This is the only way to signal the boot stage on a laptop that has
   no COM port while the screen does not work yet (or any more).
   Bits: 0 - ScrollLock, 1 - NumLock, 2 - CapsLock. */
static void kbd_wait_write(void) {
    for (u32 i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) return;      /* the input buffer is free */
}

/* Consume the ACK (0xFA) ourselves, otherwise it reaches the IRQ handler. */
static void kbd_wait_ack(void) {
    for (u32 i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) {               /* a byte is waiting in the output buffer */
            u8 r = inb(0x60);
            if (r == 0xFA || r == 0xFE) return;
        }
    }
}

/* Mirror the CapsLock state on the keyboard indicator. */
static void kbd_sync_leds(void) {
    kbd_set_leds((u8)((keymap.caps?4:0)|(mousekeys?2:0)));
}

int kbd_shift(void) { return (keymap_modifiers(&keymap)&KMOD_SHIFT)!=0; }
int kbd_layout(void) { return keymap.layout; }
void kbd_layout_set(int russian) {
    kv_flags_t f=irq_save(); keymap.layout=russian!=0; irq_restore(f);
}
int kbd_mousekeys(void) { return mousekeys; }
void kbd_mousekeys_set(int on) {
    mousekeys=on?1:0; mk_release=0;
    if(!mousekeys) mouse_set_button(0,0);
    kbd_sync_leds();
}

/* A fallback poll. On some laptops IRQ1 never reaches the PIC
   (non-standard routing, USB keyboard emulation in the BIOS). Bytes
   then pile up in the output buffer while the handler stays silent.
   The timer calls this function: when the IRQ does work the buffer is
   always empty and the call costs nothing. */
void kbd_poll(void) {
    /* The poll runs from the timer handler and may cut in between the
       status check and the read of 0x60 in the keyboard handler - the
       same scan code would then be processed twice (a doubled
       character). At 1000 Hz the odds of that race grew tenfold, so the
       read is made atomic. */
    kv_flags_t fl = irq_save();
    for (int i = 0; i < 8; i++) {
        u8 st = inb(0x64);
        if (!(st & 0x01)) break;       /* nothing buffered */
        if (st & 0x20) break;          /* a mouse byte, not ours */
        kbd_cb(0);
    }
    if(mk_release && timer_ticks()>=mk_release) { mk_release=0; mouse_set_button(0,0); }
    irq_restore(fl);
}

static u8 led_mask = 0;            /* last value written to the hardware */

void kbd_set_leds(u8 mask) {
    kv_flags_t fl = irq_save();          /* the command exchange must be atomic */
    kbd_wait_write();
    outb(0x60, 0xED);
    kbd_wait_ack();
    kbd_wait_write();
    outb(0x60, (u8)(mask & 0x07));
    kbd_wait_ack();
    led_mask = (u8)(mask & 0x07);
    irq_restore(fl);
}

/* The mask currently lit on the keyboard (bits: 1 Scroll, 2 Num,
   4 Caps). Used by the `leds` shell command to restore the state
   after a light show. */
u8 kbd_get_leds(void) {
    return led_mask;
}

/* Full initialisation of the i8042 controller.
   This used to install the IRQ1 handler and nothing else. That worked
   under QEMU because the emulator hands over a controller that is
   already enabled and translating scan codes. A real i8042 (laptops,
   Samsung RV410) may be left by the BIOS with the port disabled,
   translation off or an unread byte in the buffer - and then IRQ1 never
   arrives and there is no input at all. */
void keyboard_init(void) {
    kbd_head = kbd_tail = 0; keymap_init(&keymap);
    irq_install_handler(1, kbd_cb);

    kv_flags_t fl = irq_save();

    /* 1. Drain the leftovers from the BIOS. While a byte sits in the
          output buffer no further IRQ1 will arrive. */
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);

    /* 2. Enable the first PS/2 port (the BIOS may have disabled it). */
    kbd_wait_write();
    outb(0x64, 0xAE);

    /* 3. Read the controller configuration byte. */
    kbd_wait_write();
    outb(0x64, 0x20);
    u8 cfg = 0;
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01) { cfg = inb(0x60); break; }

    cfg |=  0x01;      /* bit 0: enable the IRQ1 interrupt */
    cfg &= (u8)~0x10;  /* bit 4: lift the keyboard clock inhibit */
    cfg |=  0x40;      /* bit 6: translation to scan code set 1 -
                          exactly what map_lower/upper expect */

    kbd_wait_write();
    outb(0x64, 0x60);
    kbd_wait_write();
    outb(0x60, cfg);

    /* 4. Allow the keyboard to report scan codes (0xF4).
          Without this command the device stays silent. */
    kbd_wait_write();
    outb(0x60, 0xF4);
    kbd_wait_ack();

    /* 5. Drain the buffer once more: command replies are of no use. */
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++)
        (void)inb(0x60);

    irq_restore(fl);
}

int kbd_getevent_nb(kbd_event_t *e) {
    if(kbd_head==kbd_tail) return 0;
    *e=buf[kbd_tail]; kbd_tail=(kbd_tail+1)%BUF_SIZE; return 1;
}
int kbd_getchar_nb(void) {
    kbd_event_t e;
    while(kbd_getevent_nb(&e)) if(e.key!=KEY_LAYOUT) return e.key;
    return -1;
}

char kbd_getchar(void) {
    int c;
    while ((c = kbd_getchar_nb()) < 0) {
        /* Before the scheduler starts task_yield() returns immediately and
           the loop burned 100% of the CPU. Wait for an interrupt for
           real. hlt is only safe with interrupts enabled - otherwise it
           would be an eternal sleep. */
        kv_addr_t fl;
        __asm__ volatile(
#ifdef __x86_64__
            "pushfq; popq %0"
#else
            "pushfl; popl %0"
#endif
            : "=r"(fl));
        if (fl & 0x200) __asm__ volatile("hlt");
        else            __asm__ volatile("pause");
        task_yield();
    }
    return (char)c;
}
