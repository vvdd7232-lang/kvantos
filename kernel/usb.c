/* ============================================================
 *  KvantOS - USB mass storage over UHCI (read-only)
 *
 *  A real USB flash drive plugged into a USB port is read as if it
 *  were an ATA disk and auto-mounted by the partition scanner. UHCI is
 *  the USB 1.0 host controller (Intel PIIX3/PIIX4); QEMU's default
 *  machine exposes one, so the whole stack is testable there.
 *
 *  Bit layouts follow the UHCI specification (see OSDev). The host
 *  controller keeps a 1 ms frame counter (FRNUM) that advances on its
 *  own, so delays and completion polls work even with CPU interrupts
 *  disabled - which matters because USB discovery runs during the boot
 *  autoscan, before sti(). No interrupts are used: every transfer is
 *  polled.
 * ============================================================ */
#include "kernel.h"
#include "vfs.h"

/* ---- UHCI register offsets (I/O space, from the PCI I/O BAR) ---- */
#define UHCI_CMD        0x00
#define UHCI_STS        0x02
#define UHCI_INTR       0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASE     0x08
#define UHCI_SOF        0x0c
#define UHCI_PORTSC1    0x10

#define CMD_RUN         0x0001
#define CMD_HCRESET     0x0002
#define CMD_MAXP64      0x0080
#define CMD_CF          0x0040

#define PORT_CCS        0x0001
#define PORT_CSC        0x0002
#define PORT_ENABLE     0x0004
#define PORT_RESET      0x0200

/* ---- Transfer Descriptor (16 bytes, 16-byte aligned) ---- */
typedef struct __attribute__((packed)) {
    u32 link;
    u32 ctrl;
    u32 token;
    u32 buf;
} uhci_td_t;

/* ---- Queue Head (8 bytes, 16-byte aligned) ---- */
typedef struct __attribute__((packed)) {
    u32 hlink;
    u32 vlink;
} uhci_qh_t;

/* ctrl/status bits */
#define TD_SPD        (1u<<29)
#define TD_CERR_3     (3u<<27)
#define TD_LS         (1u<<26)
#define TD_IOC        (1u<<24)
#define TD_ACTIVE     (1u<<23)
#define TD_STALLED    (1u<<22)
#define TD_DBUFERR    (1u<<21)
#define TD_BABBLE     (1u<<20)
#define TD_NAK        (1u<<19)
#define TD_CRC        (1u<<18)
#define TD_BITSTUFF   (1u<<17)
#define TD_ERR_ANY    (TD_STALLED|TD_DBUFERR|TD_BABBLE|TD_CRC|TD_BITSTUFF)
#define TD_ACTLEN     0x7ff

/* link-pointer bits. Note: TD next-link uses bit0=terminate,
   bit1=QH-type, bit2=depth-first. Frame/QH links use bit0=enable
   (1=empty), bit1=type. In practice bit0=1 means "stop" for both. */
#define LINK_TERM     1u
#define LINK_QH       2u
#define LINK_DEPTH    4u

/* token PIDs */
#define PID_IN        0x69
#define PID_OUT       0xE1
#define PID_SETUP     0x2D

/* ---- one discovered USB mass-storage disk ---- */
#define USB_MAX_DISKS 4
typedef struct {
    u32 sectors;        /* in 512-byte sectors          */
    u32 block_size;     /* bytes per sector             */
    u8  addr;           /* USB device address           */
    u8  bulk_in;        /* endpoint number (0x..)       */
    u8  bulk_out;
    u16 in_max;         /* max packet sizes             */
    u16 out_max;
    u8  tog_in;         /* data-toggle state per ep     */
    u8  tog_out;
    u8  iface;          /* mass-storage interface index */
    char vendor[9];
    char product[17];
} usb_disk_t;
static usb_disk_t usb_disks[USB_MAX_DISKS];
static int usb_n = 0;

/* ---- the single UHCI controller we drive ---- */
static u16 uhci_io = 0;

/* persistent async queue head: every frame list entry points here, and
   a transfer is queued by hanging its TD chain off qh.vlink. */
static uhci_qh_t *uhci_qh = NULL;
static u32       *uhci_flist = NULL;   /* 1024 entries = 4 KiB */

/* scratch pages in low (identity-mapped) memory for TDs and DMA.
   virt == phys in the first 128 MiB, so these pointers double as the
   physical addresses the controller DMAs to. */
static uhci_td_t *td_pool = NULL;      /* one page -> 256 TDs  */
static u32        td_off  = 0;
static u8        *dma_buf = NULL;      /* 4 KiB bounce buffer   */

static void pool_reset(void) { td_off = 0; }
static uhci_td_t *td_alloc(void) {
    td_off = (td_off + 15) & ~15u;
    uhci_td_t *t = (uhci_td_t *)((u8 *)td_pool + td_off);
    td_off += 16;
    return t;
}

/* ---- delays that work with interrupts disabled ----
   FRNUM advances one unit per 1 ms frame while the controller runs. */
static void wait_frames(u32 n) {
    if (!uhci_io) return;
    u16 f0 = inw(uhci_io + UHCI_FRNUM) & 0x7ff;
    for (;;) {
        u16 now = inw(uhci_io + UHCI_FRNUM) & 0x7ff;
        if ((u16)(now - f0) >= (u16)n) break;
    }
}
/* A short busy spin, used before the controller starts running. */
static u64 tsc_now(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
static void spin_us(u32 us) {
    u64 want = tsc_now() + (u64)us * 1500u;   /* ~1.5 GHz baseline, generous */
    while (tsc_now() < want) { }
}

/* ============================================================
 *  Building and running a transfer
 * ============================================================ */

/* Token for one USB transaction: pid, device, endpoint, toggle, length. */
static u32 make_token(u8 pid, u8 dev, u8 ep, u8 tog, u32 len) {
    u32 maxlen = (len == 0) ? 0x7ff : (len - 1);
    return (maxlen << 21) | ((u32)(tog & 1) << 19) | ((u32)(ep & 15) << 15)
         | ((u32)(dev & 0x7f) << 8) | pid;
}

static uhci_td_t *new_td(u8 pid, u8 dev, u8 ep, u8 tog, u32 len,
                         const void *buf, u32 ctrl_extra) {
    uhci_td_t *t = td_alloc();
    t->link  = LINK_TERM;
    t->ctrl  = TD_ACTIVE | TD_CERR_3 | ctrl_extra;
    t->token = make_token(pid, dev, ep, tog, len);
    t->buf   = buf ? (u32)(u32 *)buf : 0;
    return t;
}

/* Wait for a TD to stop being active (controller processed it). */
static int td_wait(uhci_td_t *t) {
    u16 f0 = inw(uhci_io + UHCI_FRNUM) & 0x7ff;
    while (t->ctrl & TD_ACTIVE) {
        u16 now = inw(uhci_io + UHCI_FRNUM) & 0x7ff;
        if ((u16)(now - f0) >= 200) {
            kprintf("USB: td timeout ctrl=%08x\n", t->ctrl);
            return -1;   /* 200 ms timeout */
        }
    }
    if (t->ctrl & TD_ERR_ANY) {
        kprintf("USB: td error ctrl=%08x\n", t->ctrl);
        return -1;
    }
    return 0;
}

/* Queue a chain of TDs under the async QH and wait for the last one. */
static int run_chain(uhci_td_t *first, uhci_td_t *last) {
    last->ctrl |= TD_IOC;
    uhci_qh->vlink = ((u32)(u32 *)first);   /* TD type, valid */
    int rc = td_wait(last);
    if (rc < 0)
        kprintf("USB: setup-td ctrl=%08x tok=%08x buf=%08x\n",
                first->ctrl, first->token, first->buf);
    uhci_qh->vlink = LINK_TERM;
    return rc;
}

/* ---- a control transfer on endpoint 0 ----
   in = 1: data phase is device->host (IN); in = 0: host->device (OUT).
   wLen bytes are read into / written from `data`. Returns bytes moved
   or -1. */
static int usb_control(u8 dev, int low, u8 rtype, u8 req,
                       u16 wval, u16 widx, u16 wlen, void *data, int in) {
    static u8 setup_pkt[8] __attribute__((aligned(16)));
    pool_reset();

    setup_pkt[0] = rtype; setup_pkt[1] = req;
    setup_pkt[2] = (u8)wval; setup_pkt[3] = (u8)(wval >> 8);
    setup_pkt[4] = (u8)widx; setup_pkt[5] = (u8)(widx >> 8);
    setup_pkt[6] = (u8)wlen; setup_pkt[7] = (u8)(wlen >> 8);

    /* SETUP is always DATA0. A low-speed device needs the LS bit set on
       every TD of the transfer. */
    u32 ls = low ? TD_LS : 0;
    uhci_td_t *setup = new_td(PID_SETUP, dev, 0, 0, 8, setup_pkt, ls);

    /* Control data phase as a SINGLE TD carrying the whole payload. A
       UHCI IN TD accumulates successive packets into one buffer until
       the requested length is reached or the device ends the transfer
       with a short packet, so one TD covers any descriptor. We must NOT
       enable SPD here: SPD halts the queue on a short packet, which
       would skip the status phase and time out every descriptor read.
       Without SPD the controller advances to the status TD normally. */
    uhci_td_t *prev = setup;
    if (wlen) {
        u8 pid = in ? PID_IN : PID_OUT;
        uhci_td_t *d = new_td(pid, dev, 0, 1, wlen, data, ls);   /* DATA1 */
        prev->link = ((u32)(u32 *)d) | LINK_DEPTH;               /* depth-first */
        prev = d;
    }

    /* status phase: zero length, opposite direction, DATA1 */
    u8 spid = in ? PID_OUT : PID_IN;
    uhci_td_t *status = new_td(spid, dev, 0, 1, 0, NULL, ls);
    prev->link = ((u32)(u32 *)status) | LINK_DEPTH;

    int rc = run_chain(setup, status);
    if (rc < 0) return -1;

    /* actual bytes received on the data TDs */
    if (in && wlen) {
        uhci_td_t *d = (uhci_td_t *)(((u32)(u32 *)setup->link) & ~15u);
        u32 got = 0;
        while (d != status) {
            got += ((d->ctrl & TD_ACTLEN) + 1) & 0x7ff;
            u32 nx = d->link & ~15u;
            if (d->link & LINK_TERM) break;
            d = (uhci_td_t *)nx;
        }
        return (int)got;
    }
    return wlen;
}

/* ============================================================
 *  Mass storage (USB "Bulk-Only Transport") + SCSI, read-only
 * ============================================================ */

static int msd_cbw(usb_disk_t *d, const u8 *cb, u8 cblen, u32 dlen, int in) {
    static u8 cbw[31] __attribute__((aligned(16)));
    static u32 tag = 0;
    cbw[0]=0x55; cbw[1]=0x53; cbw[2]=0x42; cbw[3]=0x43;        /* USBC */
    cbw[4]=tag; cbw[5]=tag>>8; cbw[6]=tag>>16; cbw[7]=tag>>24; tag++;
    cbw[8]=dlen; cbw[9]=dlen>>8; cbw[10]=dlen>>16; cbw[11]=dlen>>24;
    cbw[12] = in ? 0x80 : 0x00;                                /* IN flag */
    cbw[13] = 0;                                               /* LUN */
    cbw[14] = cblen;
    for (int i = 0; i < 16; i++) cbw[15 + i] = i < cblen ? cb[i] : 0;

    pool_reset();
    uhci_td_t *t = new_td(PID_OUT, d->addr, d->bulk_out, d->tog_out, 31, cbw, 0);
    int rc = run_chain(t, t);
    if (rc == 0) d->tog_out ^= 1;
    return rc;
}

static int msd_csw(usb_disk_t *d) {
    static u8 csw[13] __attribute__((aligned(16)));
    pool_reset();
    uhci_td_t *t = new_td(PID_IN, d->addr, d->bulk_in, d->tog_in, 13, csw, TD_SPD);
    int rc = run_chain(t, t);
    if (rc == 0) d->tog_in ^= 1;
    if (rc < 0) return -1;
    return csw[12] == 0 ? 0 : -1;     /* status byte: 0 = command OK */
}

/* Bulk IN of exactly `len` bytes (len <= 4096) into the low dma buffer. */
static int msd_bulk_in(usb_disk_t *d, u32 len) {
    if (!len) return 0;
    pool_reset();
    uhci_td_t *first = NULL, *prev = NULL;
    u32 off = 0, remaining = len, tog = d->tog_in;
    while (remaining) {
        u32 chunk = remaining > d->in_max ? d->in_max : remaining;
        uhci_td_t *t = new_td(PID_IN, d->addr, d->bulk_in, tog, chunk,
                              dma_buf + off, TD_SPD);
        if (!first) first = t; else prev->link = ((u32)(u32 *)t) | LINK_DEPTH;
        prev = t; tog ^= 1; off += chunk; remaining -= chunk;
    }
    int rc = run_chain(first, prev);
    if (rc == 0) d->tog_in = tog;
    return rc;
}

static void msd_reset_toggles(usb_disk_t *d) {
    /* Bulk-Only Mass Storage Reset (class request 0xFF on the
       interface), then CLEAR_FEATURE(ENDPOINT_HALT) on both bulk
       endpoints. For CLEAR_FEATURE: bmRequestType 0x02 (standard, OUT,
       endpoint recipient), bRequest 0x01, wValue = feature selector
       (ENDPOINT_HALT = 0), wIndex = endpoint address - the IN endpoint
       keeps its direction bit. Both actions reset the device's data
       toggle to DATA0, so the host side is set to match. */
    usb_control(d->addr, 0, 0x21, 0xFF, 0, d->iface, 0, NULL, 0);
    usb_control(d->addr, 0, 0x02, 0x01, 0, 0x80u | d->bulk_in,  0, NULL, 0);
    usb_control(d->addr, 0, 0x02, 0x01, 0, d->bulk_out, 0, NULL, 0);
    d->tog_in = 0; d->tog_out = 0;
}

/* Run one BOT command with a DATA-IN phase of dlen bytes. */
static int msd_in_cmd(usb_disk_t *d, const u8 *cb, u8 cblen, u32 dlen) {
    if (msd_cbw(d, cb, cblen, dlen, 1) < 0) return -1;
    if (msd_bulk_in(d, dlen) < 0) { msd_reset_toggles(d); return -1; }
    if (msd_csw(d) < 0) return -1;
    return 0;
}

static int msd_no_data_cmd(usb_disk_t *d, const u8 *cb, u8 cblen) {
    if (msd_cbw(d, cb, cblen, 0, 0) < 0) return -1;
    return msd_csw(d);
}

static int msd_test_unit_ready(usb_disk_t *d) {
    u8 cb[6] = {0};
    for (int t = 0; t < 5; t++) {
        u8 r = msd_no_data_cmd(d, cb, 6);
        if (r == 0) return 0;
        msd_reset_toggles(d);
        wait_frames(10);
    }
    return -1;
}

static int msd_read_capacity(usb_disk_t *d) {
    u8 cb[10] = {0x25, 0};
    if (msd_in_cmd(d, cb, 10, 8) < 0) return -1;
    u32 lba  = (dma_buf[0]<<24)|(dma_buf[1]<<16)|(dma_buf[2]<<8)|dma_buf[3];
    u32 bsz  = (dma_buf[4]<<24)|(dma_buf[5]<<16)|(dma_buf[6]<<8)|dma_buf[7];
    d->block_size = bsz ? bsz : 512;
    d->sectors = lba + 1;
    return 0;
}

/* ============================================================
 *  Enumeration
 * ============================================================ */

/* Parse a configuration descriptor blob for the mass-storage bulk
   endpoints. Returns 1 if found. */
static int find_bulk_eps(const u8 *cfg, u32 len, usb_disk_t *d) {
    u32 i = 0;
    d->bulk_in = d->bulk_out = 0; d->in_max = d->out_max = 64;
    while (i + 8 <= len) {
        u8 dt = cfg[i], dl = cfg[i + 1];
        if (dl < 8 || i + dl > len) break;
        if (dt == 5) {                 /* endpoint descriptor */
            u8  ea  = cfg[i + 2];
            u8  attr= cfg[i + 3];
            u16 mps= cfg[i + 4] | (cfg[i + 5] << 8);
            if (attr == 2) {           /* bulk */
                if (ea & 0x80) { d->bulk_in = ea & 0x0f; d->in_max  = mps; }
                else          { d->bulk_out= ea & 0x0f; d->out_max = mps; }
            }
        } else if (dt == 4) {          /* interface descriptor */
            d->iface = cfg[i + 2];
        }
        i += dl;
    }
    return (d->bulk_in || d->bulk_out) ? 1 : 0;
}

static void enumerate_port(int port, int low) {
    u8 desc[18] __attribute__((aligned(16)));

    kprintf("USB: port %d %s-speed, probing device...\n",
            port, low ? "low" : "full");

    int n = -1, attempt;
    for (attempt = 0; attempt < 6; attempt++) {
        n = usb_control(0, low, 0x80, 0x06, 0x0100, 0, 18, desc, 1);
        if (n >= 8) break;
        wait_frames(25);
    }
    if (attempt > 0 && n >= 8)
        kprintf("USB: descriptor on attempt %d\n", attempt + 1);
    if (n < 8) { kprintf("USB: no descriptor on port %d\n", port); return; }

    /* Assign an address. */
    u8 addr = (u8)(usb_n + 2);          /* keep 1 free for keyboards later */
    if (usb_control(0, low, 0x00, 0x05, addr, 0, 0, NULL, 0) < 0) {
        kprintf("USB: SET_ADDRESS failed\n"); return;
    }
    wait_frames(2);

    /* Re-read the full device descriptor at the new address. */
    n = usb_control(addr, low, 0x80, 0x06, 0x0100, 0, 18, desc, 1);
    if (n < 18) { kprintf("USB: dev descriptor short\n"); return; }

    u16 vid = desc[8] | (desc[9] << 8);
    u16 pid = desc[10] | (desc[11] << 8);
    kprintf("USB: device vid=%04x pid=%04x class=%d\n", vid, pid, desc[4]);

    /* Read the configuration descriptor (with following interface +
       endpoint descriptors) and set configuration 1. */
    static u8 cfg[256] __attribute__((aligned(16)));
    int cl = usb_control(addr, low, 0x80, 0x06, 0x0200, 0, 255, cfg, 1);
    usb_control(addr, low, 0x00, 0x09, 1, 0, 0, NULL, 0);

    if (cl < 18) { kprintf("USB: no config descriptor\n"); return; }

    /* Is it mass storage (class 8, subclass 6, protocol 0x50 = BOT)? */
    u8 icls = cfg[14], isub = cfg[15], ipro = cfg[16];
    kprintf("USB: iface class=%d sub=%d proto=%d\n", icls, isub, ipro);
    if (!(icls == 8 && ipro == 0x50)) {
        kprintf("USB: not mass-storage BOT, skipping\n");
        return;
    }
    if (usb_n >= USB_MAX_DISKS) return;

    usb_disk_t *d = &usb_disks[usb_n];
    memset(d, 0, sizeof(*d));
    d->addr = addr;
    if (!find_bulk_eps(cfg, cl, d)) { kprintf("USB: no bulk endpoints\n"); return; }
    d->tog_in = d->tog_out = 0;   /* SET_CONFIGURATION reset toggles to DATA0 */

    kprintf("USB: mass storage epIN=%02x epOUT=%02x\n", d->bulk_in|0x80, d->bulk_out);

    msd_reset_toggles(d);
    if (msd_test_unit_ready(d) < 0) { kprintf("USB: not ready\n"); return; }
    if (msd_read_capacity(d) < 0)   { kprintf("USB: read capacity failed\n"); return; }

    kprintf("USB: disk ready, %u sectors x %u bytes\n", d->sectors, d->block_size);
    usb_n++;
}

/* ============================================================
 *  Controller bring-up
 * ============================================================ */

static int uhci_start(void) {
    /* find the UHCI controller: PCI class 0x0C, subclass 0x03, prog_if 0x00 */
    for (u32 i = 0; i < pci_count(); i++) {
        pci_dev_t *c = pci_get(i);
        if (!c || c->class_code != 0x0C || c->subclass != 0x03 || c->prog_if != 0x00)
            continue;

        /* I/O BAR */
        u16 io = 0;
        for (int b = 0; b < 6; b++)
            if (c->bar_is_io[b]) { io = (u16)(c->bar[b] & ~0xF); break; }
        if (!io) continue;
        uhci_io = io;

        /* take ownership + enable I/O and bus mastering */
        pci_write32(c->bus, c->slot, c->func, 0xC0, 0x2000);     /* disable BIOS legacy */
        u32 cmd = pci_read32(c->bus, c->slot, c->func, 0x04);
        pci_write32(c->bus, c->slot, c->func, 0x04, cmd | 0x05); /* IO + bus master */

        outw(io + UHCI_INTR, 0);

        /* host controller reset (self-clears) */
        outw(io + UHCI_CMD, CMD_HCRESET);
        for (int t = 0; t < 1000; t++)
            if (!(inw(io + UHCI_CMD) & CMD_HCRESET)) break;
        spin_us(5000);

        /* frame list + async QH, all in identity-mapped low memory */
        uhci_flist = (u32 *)pmm_alloc_frame();
        uhci_qh    = (uhci_qh_t *)pmm_alloc_frame();   /* 16-byte aligned page */
        td_pool    = (uhci_td_t *)pmm_alloc_frame();
        dma_buf    = (u8 *)pmm_alloc_frame();
        if (!uhci_flist || !uhci_qh || !td_pool || !dma_buf) {
            kprintf("USB: out of low memory\n"); return -1;
        }
        uhci_qh->hlink = LINK_TERM;
        uhci_qh->vlink = LINK_TERM;
        for (int f = 0; f < 1024; f++)
            uhci_flist[f] = ((u32)(u32 *)uhci_qh) | LINK_QH;   /* QH, valid */

        outl(io + UHCI_FLBASE, (u32)(u32 *)uhci_flist);
        outw(io + UHCI_FRNUM, 0);
        outb(io + UHCI_SOF, 0x40);

        /* run */
        outw(io + UHCI_STS, 0xFFFF);
        outw(io + UHCI_CMD, CMD_CF | CMD_MAXP64 | CMD_RUN);
        spin_us(2000);

        kprintf("USB: UHCI controller at io %04x, %d ports\n", io, 2);
        return 0;
    }
    return -1;     /* no UHCI controller */
}

void usb_init(void) {
    if (uhci_start() < 0) return;

    /* give attached devices a moment, then walk the ports */
    wait_frames(20);
    for (int port = 0; port < 2; port++) {
        u16 psc = inw(uhci_io + UHCI_PORTSC1 + port * 2);
        if (!(psc & PORT_CCS)) continue;          /* nothing plugged in */
        outw(uhci_io + UHCI_PORTSC1 + port * 2, PORT_CSC);   /* clear change */

        /* reset the port */
        outw(uhci_io + UHCI_PORTSC1 + port * 2, PORT_RESET);
        wait_frames(50);
        outw(uhci_io + UHCI_PORTSC1 + port * 2, 0);
        wait_frames(10);
        outw(uhci_io + UHCI_PORTSC1 + port * 2, PORT_ENABLE);
        wait_frames(20);

        psc = inw(uhci_io + UHCI_PORTSC1 + port * 2);
        int low = (psc & 0x0100) ? 1 : 0;
        kprintf("USB: port %d PSC=%04x (%s-speed, %s)\n", port, psc,
                low ? "low" : "full", (psc & PORT_ENABLE) ? "enabled" : "DISABLED");
        if (!(psc & PORT_ENABLE)) { kprintf("USB: port %d enable failed\n", port); continue; }

        enumerate_port(port, low);
    }
    kprintf("USB: %d mass-storage disk(s)\n", usb_n);
}

/* ---- block-device interface used by the partition scanner ---- */
int  usb_disk_count(void)        { return usb_n; }
u32  usb_sectors(int i)          { return (i >= 0 && i < usb_n) ? usb_disks[i].sectors : 0; }
const char *usb_model(int i) {
    return (i >= 0 && i < usb_n) ? "USB flash drive" : "no";
}

int usb_read(int i, u32 lba, u8 count, void *buf) {
    if (i < 0 || i >= usb_n) return -1;
    usb_disk_t *d = &usb_disks[i];
    if (d->block_size != 512) return -1;     /* only 512-byte sectors wired up */
    for (u8 s = 0; s < count; s++) {
        u8 cb[10] = { 0x28, 0,
            (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, 0, 1, 0 };
        if (msd_cbw(d, cb, 10, 512, 1) < 0)         return -1;
        if (msd_bulk_in(d, 512) < 0) { msd_reset_toggles(d); return -1; }
        if (msd_csw(d) < 0)          return -1;
        memcpy((u8 *)buf + s * 512, dma_buf, 512);
        lba++;
    }
    return 0;
}
