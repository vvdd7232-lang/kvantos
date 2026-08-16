/* PS/2 keyboard, scan code set 1, ring buffer */
#include "kernel.h"

#define BUF_SIZE 256
static volatile char buf[BUF_SIZE];
static volatile u32 head = 0, tail = 0;
static int shift = 0, ctrl = 0, caps = 0, extended = 0;
static void kbd_sync_leds(void);   /* defined below */

static const char map_lower[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0
};

static const char map_upper[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0
};

static void push(char c) {
    u32 n = (head + 1) % BUF_SIZE;
    if (n != tail) { buf[head] = c; head = n; }
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

    if (sc == 0xE0) { extended = 1; return; }

    if (extended) {
        extended = 0;
        if (!(sc & 0x80)) {
            switch (sc) {
                case 0x48: push((char)(u8)KEY_UP); return;
                case 0x50: push((char)(u8)KEY_DOWN); return;
                case 0x4B: push((char)(u8)KEY_LEFT); return;
                case 0x4D: push((char)(u8)KEY_RIGHT); return;
                case 0x47: push((char)(u8)KEY_HOME); return;
                case 0x4F: push((char)(u8)KEY_END); return;
                case 0x49: push((char)(u8)KEY_PGUP); return;
                case 0x51: push((char)(u8)KEY_PGDN); return;
                case 0x53: push((char)(u8)KEY_DEL); return;
                case 0x52: push((char)(u8)KEY_INS); return;
            }
        }
        return;
    }

    if (sc & 0x80) {
        u8 code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) shift = 0;
        if (code == 0x1D) ctrl = 0;
        return;
    }

    switch (sc) {
        case 0x2A: case 0x36: shift = 1; return;
        case 0x1D: ctrl = 1; return;
        /* the grey navigation block, and the same keys on the numeric
           keypad when NumLock is off (they arrive without the 0xE0) */
        case 0x47: push((char)(u8)KEY_HOME); return;
        case 0x4F: push((char)(u8)KEY_END);  return;
        case 0x49: push((char)(u8)KEY_PGUP); return;
        case 0x51: push((char)(u8)KEY_PGDN); return;
        case 0x53: push((char)(u8)KEY_DEL);  return;
        /* function keys F1..F10 are contiguous from 0x3B */
        case 0x3B: push((char)(u8)KEY_F1); return;
        case 0x3C: push((char)(u8)KEY_F2); return;
        case 0x3D: push((char)(u8)KEY_F3); return;
        case 0x3E: push((char)(u8)KEY_F4); return;
        case 0x3F: push((char)(u8)KEY_F5); return;
        case 0x40: push((char)(u8)KEY_F6); return;
        case 0x41: push((char)(u8)KEY_F7); return;
        case 0x42: push((char)(u8)KEY_F8); return;
        case 0x43: push((char)(u8)KEY_F9); return;
        case 0x44: push((char)(u8)KEY_F10); return;
        case 0x3A:                       /* CapsLock */
            caps = !caps;
            kbd_sync_leds();
            return;
    }

    if (sc >= 128) return;
    /* CapsLock affects letters ONLY. This used to be shift^caps for
       every key, so with CapsLock on the digit 1 produced "!" while
       Shift+1 produced "1" instead. */
    char lo = map_lower[sc];
    int letter = (lo >= 'a' && lo <= 'z');
    int up = letter ? (shift ^ caps) : shift;
    char c = up ? map_upper[sc] : lo;
    if (!c) return;
    if (ctrl) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
    }
    push(c);
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
    kbd_set_leds((u8)(caps ? 0x04 : 0x00));
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
    u32 fl = irq_save();
    for (int i = 0; i < 8; i++) {
        u8 st = inb(0x64);
        if (!(st & 0x01)) break;       /* nothing buffered */
        if (st & 0x20) break;          /* a mouse byte, not ours */
        kbd_cb(0);
    }
    irq_restore(fl);
}

void kbd_set_leds(u8 mask) {
    u32 fl = irq_save();          /* the command exchange must be atomic */
    kbd_wait_write();
    outb(0x60, 0xED);
    kbd_wait_ack();
    kbd_wait_write();
    outb(0x60, (u8)(mask & 0x07));
    kbd_wait_ack();
    irq_restore(fl);
}

/* Full initialisation of the i8042 controller.
   This used to install the IRQ1 handler and nothing else. That worked
   under QEMU because the emulator hands over a controller that is
   already enabled and translating scan codes. A real i8042 (laptops,
   Samsung RV410) may be left by the BIOS with the port disabled,
   translation off or an unread byte in the buffer - and then IRQ1 never
   arrives and there is no input at all. */
void keyboard_init(void) {
    head = tail = 0;
    irq_install_handler(1, kbd_cb);

    u32 fl = irq_save();

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

int kbd_getchar_nb(void) {
    if (head == tail) return -1;
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return (int)(u8)c;
}

char kbd_getchar(void) {
    int c;
    while ((c = kbd_getchar_nb()) < 0) {
        /* Before the scheduler starts task_yield() returns immediately and
           the loop burned 100% of the CPU. Wait for an interrupt for
           real. hlt is only safe with interrupts enabled - otherwise it
           would be an eternal sleep. */
        u32 fl;
        __asm__ volatile("pushfl; popl %0" : "=r"(fl));
        if (fl & 0x200) __asm__ volatile("hlt");
        else            __asm__ volatile("pause");
        task_yield();
    }
    return (char)c;
}
