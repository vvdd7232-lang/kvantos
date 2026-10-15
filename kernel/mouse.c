/* ============================================================
 *  KvantOS - PS/2 mouse driver (IRQ12, PS/2 and IntelliMouse packets)
 * ============================================================ */
#include "kernel.h"

static volatile i32 mx = 0, my = 0;
static i32 lim_w = 640, lim_h = 480;
static volatile u8 buttons = 0;
static u8 physical_buttons, synthetic_buttons;
static volatile i32 wheel;
static u8 packet_len=3;
static u8  packet[4];
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

    if(!phase && !(b&0x08)) return;
    packet[phase++]=b;
    if(phase<packet_len) return;
    phase=0;
    u8 f=packet[0];
    if(f&0xC0) return; /* overflow */
    i32 dx=(i32)packet[1]-((f&0x10)?256:0);
    i32 dy=(i32)packet[2]-((f&0x20)?256:0);
    mx+=dx; my-=dy;
    if(mx<0) mx=0;
    if(my<0) my=0;
    if(mx>=lim_w) mx=lim_w-1;
    if(my>=lim_h) my=lim_h-1;
    physical_buttons=f&7;
    buttons=physical_buttons|synthetic_buttons;
    if(packet_len==4) {
        int d=(int)(signed char)packet[3];
        wheel+=d;
        if(wheel>120) wheel=120;
        if(wheel< -120) wheel=-120;
    }
    events++;
}

void mouse_init(i32 w, i32 h) {
    kv_flags_t fl=irq_save();
    lim_w = w; lim_h = h;
    mx = w / 2; my = h / 2;
    phase = buttons = physical_buttons = synthetic_buttons = 0;
    wheel=0; packet_len=3;

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
    /* IntelliMouse handshake. A plain PS/2 mouse returns ID 0 and keeps
       three-byte packets; an ID 3 device adds a signed wheel byte. */
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    mouse_cmd(0xF2); wait_read();
    u8 id=inb(0x60);
    if(id==3) packet_len=4;
    irq_install_handler(12, mouse_cb);
    mouse_cmd(0xF4);        /* enable packet reporting */
    present = 1;
    irq_restore(fl);
}

int  mouse_present(void)  { return present; }
i32  mouse_x(void)        { return mx; }
i32  mouse_y(void)        { return my; }
u8   mouse_buttons(void)  { return buttons; }
u32  mouse_events(void)   { return events; }

int mouse_wheel_delta(void) {
    kv_flags_t fl=irq_save(); int d=wheel; wheel=0; irq_restore(fl); return d;
}
void mouse_move_rel(i32 dx,i32 dy) {
    kv_flags_t fl=irq_save();
    mx+=dx; my+=dy;
    if(mx<0) mx=0;
    if(my<0) my=0;
    if(mx>=lim_w) mx=lim_w-1;
    if(my>=lim_h) my=lim_h-1;
    events++; irq_restore(fl);
}
void mouse_set_button(int btn,int down) {
    kv_flags_t fl=irq_save(); u8 mask=(u8)(1u<<(btn&3));
    if(down) synthetic_buttons|=mask; else synthetic_buttons&=(u8)~mask;
    buttons=physical_buttons|synthetic_buttons; events++; irq_restore(fl);
}
void mouse_set_bounds(i32 w,i32 h) {
    kv_flags_t fl=irq_save(); lim_w=w; lim_h=h;
    if(mx>=w) mx=w-1;
    if(my>=h) my=h-1;
    irq_restore(fl);
}
void mouse_set_pos(i32 x,i32 y) {
    kv_flags_t fl=irq_save(); mx=x; my=y; irq_restore(fl);
}
