/* ============================================================
 *  KvantOS - PS/2 mouse driver (IRQ12, 3-byte packets)
 * ============================================================ */
#include "kernel.h"

static i32 mx = 0, my = 0;
static i32 lim_w = 640, lim_h = 480;
static u8  buttons = 0;
static u8  packet[3];
static u8  phase = 0;
static int present = 0;
static volatile u32 events = 0;

static void wait_write(void) { int g = 100000; while (g-- && (inb(0x64) & 2)); }
static void wait_read(void)  { int g = 100000; while (g-- && !(inb(0x64) & 1)); }

static void mouse_cmd(u8 cmd) {
    wait_write(); outb(0x64, 0xD4);
    wait_write(); outb(0x60, cmd);
    wait_read();  inb(0x60);            /* ACK */
}

static void mouse_cb(registers_t *r) {
    (void)r;
    u8 status = inb(0x64);
    if (!(status & 0x01) || !(status & 0x20)) return;   /* the data did not come from the mouse */
    u8 b = inb(0x60);

    switch (phase) {
        case 0:
            if (!(b & 0x08)) return;    /* sync bit lost - wait for the packet start */
            packet[0] = b; phase = 1; break;
        case 1:
            packet[1] = b; phase = 2; break;
        case 2: {
            packet[2] = b; phase = 0;
            u8 f = packet[0];
            if (f & 0xC0) break;        /* overflow - the packet is dropped */
            i32 dx = (i32)packet[1] - ((f & 0x10) ? 256 : 0);
            i32 dy = (i32)packet[2] - ((f & 0x20) ? 256 : 0);
            mx += dx;
            my -= dy;                   /* the screen Y axis points downwards */
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > lim_w - 1) mx = lim_w - 1;
            if (my > lim_h - 1) my = lim_h - 1;
            buttons = f & 0x07;
            events++;
            break;
        }
    }
}

void mouse_init(i32 w, i32 h) {
    lim_w = w; lim_h = h;
    mx = w / 2; my = h / 2;
    phase = 0; buttons = 0;

    wait_write(); outb(0x64, 0xA8);                 /* enable the second port */
    wait_write(); outb(0x64, 0x20);                 /* read the configuration */
    wait_read();
    u8 cfg = inb(0x60);
    cfg |= 0x02;                                    /* allow IRQ12 */
    cfg &= (u8)~0x20;                               /* lift the clock inhibit */
    /* The configuration byte is shared by both ports. The mouse used to
       clear the keyboard bits by accident (on a real i8042 they are not
       guaranteed to be set after BIOS) - and input disappeared entirely.
       They are re-asserted explicitly here. */
    cfg |= 0x01;                                    /* keyboard IRQ1 */
    cfg &= (u8)~0x10;                               /* keyboard clock */
    cfg |= 0x40;                                    /* translation to set 1 */
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cfg);

    mouse_cmd(0xF6);        /* default settings */
    mouse_cmd(0xF4);        /* enable packet reporting */

    irq_install_handler(12, mouse_cb);
    present = 1;
}

int  mouse_present(void)  { return present; }
i32  mouse_x(void)        { return mx; }
i32  mouse_y(void)        { return my; }
u8   mouse_buttons(void)  { return buttons; }
u32  mouse_events(void)   { return events; }

void mouse_set_bounds(i32 w, i32 h) {
    lim_w = w; lim_h = h;
    if (mx >= w) mx = w - 1;
    if (my >= h) my = h - 1;
}

void mouse_set_pos(i32 x, i32 y) { mx = x; my = y; }
