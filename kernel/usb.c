/* UHCI (full speed) / EHCI (high speed), root-port BOT/SCSI storage, read-only.
 * No hubs, OHCI/xHCI, UAS or non-512-byte media. All DMA is static,
 * below 4 GiB, packetised at endpoint MPS; no IRQ or timer dependency. */
#include "kernel.h"

#define HC_MAX 4
#define USB_MAX 8
#define TD_MAX 512
#define ACTIVE (1u << 23)
#define TD_ERRORS 0x007E0000u
#define TERM 1u
#define IN 0x69u
#define OUT 0xE1u
#define SETUP 0x2Du

typedef struct { volatile u32 link, status; u32 token, buffer; } td_t;
typedef struct { volatile u32 link, element; u32 pad[2]; } qh_t;
typedef struct {
    volatile u32 *op;
    u16 io;
    u8 kind, ports, dead, bus, slot, func; /* kind: 0 UHCI, 1 EHCI */
} hc_t;
typedef struct {
    hc_t *hc;
    u32 sectors;
    u16 in_mps, out_mps;
    u8 address, port, ep0, in, out, itoggle, otoggle, iface;
} usb_disk_t;
static hc_t hcs[HC_MAX];
static usb_disk_t disks[USB_MAX];
static int hc_count, disk_count, initialised, ehci_count, uhci_count, ehci_seen;
static u32 frames[HC_MAX][1024] __attribute__((aligned(4096)));
static qh_t queues[HC_MAX] __attribute__((aligned(16)));
static td_t tds[TD_MAX] __attribute__((aligned(16)));
static u8 dma_buf[4096] __attribute__((aligned(4096)));
static u8 setup_buf[8] __attribute__((aligned(16)));
static u8 config[1024], reply[64];
static u32 next_tag;
/* Read-only sector cache also prevents readdir(index) from rereading a FAT
 * directory for every row. Invalidated on every reprobe, never caches writes. */
static struct { u8 data[512]; u32 lba; int disk, valid; } cache[32];
static unsigned cache_next;

static u32 physical(const void *p) { return paging_phys((u32)(kv_addr_t)p); }
static u64 clock_now(void) {
    u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
static int frames_wait(hc_t *h, unsigned n);
#include "usb_ehci.h"
static unsigned frame_number(hc_t *h) {
    return h->kind ? (ehci_reg(h, 0x0C) >> 3) & 2047 : inw(h->io + 6) & 2047;
}
static u32 port_status(hc_t *h, int p) {
    return h->kind ? ehci_reg(h, 0x44 + 4*p) : inw(h->io + 0x10 + 2*p);
}
/* Both frame counters wrap at 2048 milliseconds. TSC bounds a stopped HC. */
static int frames_wait(hc_t *h, unsigned n) {
    unsigned start = frame_number(h);
    u64 until = clock_now() + 1000000000ULL;
    while (((frame_number(h) - start) & 2047u) < n) {
        if (h->dead || (h->kind ? ehci_reg(h,4) & 0x1000 : inw(h->io+2) & 0x20) || clock_now() > until) return 0;
        __asm__ volatile("pause");
    }
    return 1;
}
static u32 token(u8 pid, usb_disk_t *d, u8 ep, int toggle, unsigned len) {
    return pid | ((u32)d->address << 8) | ((u32)ep << 15) |
           ((u32)toggle << 19) | (((len - 1) & 2047) << 21);
}
/* Execute a chain and return actual bytes. Short IN packets terminate the
 * transfer. Detach and let two frames pass before reusing descriptors. */
static int transfer(usb_disk_t *d, u8 pid, u8 ep, u16 mps,
                    int *toggle, void *buf, unsigned len) {
    if (d->hc->dead || !mps || len > sizeof(dma_buf)) return -1;
    if (d->hc->kind) return ehci_transfer(d,pid,ep,mps,toggle,buf,len);
    unsigned count = len ? (len + mps - 1) / mps : 1;
    if (count > TD_MAX) return -1;
    if (pid != IN && len) memcpy(dma_buf, buf, len);
    unsigned off = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned bytes = len - off;
        if (bytes > mps) bytes = mps;
        tds[i].link = i + 1 < count ? physical(&tds[i + 1]) | 4 : TERM;
        tds[i].status = ACTIVE | (3u << 27) | (pid == IN ? 1u << 29 : 0);
        tds[i].token = token(pid, d, ep, (*toggle + i) & 1, bytes);
        tds[i].buffer = physical(dma_buf + off);
        off += bytes;
    }
    qh_t *q = &queues[d->hc - hcs];
    __asm__ volatile("" ::: "memory");
    q->element = physical(tds);
    unsigned start = inw(d->hc->io + 6) & 2047, done = 0;
    int result = -1, total = 0;
    u64 until = clock_now() + 1000000000ULL;
    while (((inw(d->hc->io + 6) - start) & 2047u) < 250 && clock_now() < until) {
        if (!(inw(d->hc->io + 0x10 + 2 * d->port) & 1)) break;
        while (done < count) {
            u32 status = tds[done].status;
            if (status & ACTIVE) break;
            if (status & TD_ERRORS) goto detach;
            unsigned actual = (status + 1) & 2047;
            unsigned expected = ((tds[done].token >> 21) + 1) & 2047;
            if (actual > expected) goto detach;
            total += actual; done++;
            if (actual < expected || done == count) { result = total; goto detach; }
        }
        if (inw(d->hc->io + 2) & 0x20) break;
        __asm__ volatile("pause");
    }
detach:
    q->element = TERM;
    __asm__ volatile("" ::: "memory");
    if (!frames_wait(d->hc, 2)) return -1;
    if (result >= 0) {
        *toggle = (*toggle + done) & 1;
        if (pid == IN && result) memcpy(buf, dma_buf, (u32)result);
    }
    return result;
}
static int control(usb_disk_t *d, u8 type, u8 req, u16 value, u16 index,
                   void *buf, u16 len) {
    setup_buf[0] = type; setup_buf[1] = req;
    setup_buf[2] = value; setup_buf[3] = value >> 8;
    setup_buf[4] = index; setup_buf[5] = index >> 8;
    setup_buf[6] = len; setup_buf[7] = len >> 8;
    int toggle = 0;
    if (transfer(d, SETUP, 0, d->ep0, &toggle, setup_buf, 8) != 8) return -1;
    int actual = 0;
    toggle = 1;
    if (len) {
        actual = transfer(d, type & 0x80 ? IN : OUT, 0, d->ep0, &toggle, buf, len);
        if (actual < 0) return -1;
    }
    toggle = 1;
    if (transfer(d, type & 0x80 ? OUT : IN, 0, d->ep0, &toggle, reply, 0) != 0) return -1;
    return actual;
}
static void put32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = v >> (8*i); }
static u32 get32(const u8 *p) { return p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }
static u32 be32(const u8 *p) { return p[3] | ((u32)p[2]<<8) | ((u32)p[1]<<16) | ((u32)p[0]<<24); }
static void recover(usb_disk_t *d) {
    control(d, 0x21, 0xFF, 0, d->iface, 0, 0); /* BOT reset */
    control(d, 2, 1, 0, d->in | 0x80, 0, 0); /* clear halt */
    control(d, 2, 1, 0, d->out, 0, 0);
    d->itoggle = d->otoggle = 0;
}
/* No SCSI write commands exist in this driver. LUN 0 only. */
static int command(usb_disk_t *d, const u8 *cdb, u8 cdb_len, void *buf, u32 size) {
    u8 cbw[31], csw[13];
    memset(cbw, 0, sizeof(cbw));
    u32 tag = ++next_tag;
    put32(cbw, 0x43425355); put32(cbw+4, tag); put32(cbw+8, size);
    cbw[12] = 0x80; cbw[14] = cdb_len; memcpy(cbw+15, cdb, cdb_len);
    /* Keep toggles as integers locally: no unaligned/int access to u8 state. */
    int ot = d->otoggle, it = d->itoggle;
    if (transfer(d, OUT, d->out, d->out_mps, &ot, cbw, 31) != 31) goto bad;
    int bytes = size ? transfer(d, IN, d->in, d->in_mps, &it, buf, size) : 0;
    if (bytes < 0 || transfer(d, IN, d->in, d->in_mps, &it, csw, 13) != 13) goto bad;
    d->otoggle = ot; d->itoggle = it;
    if (get32(csw) != 0x53425355 || get32(csw+4) != tag || csw[12] > 1 ||
        get32(csw+8) > size || get32(csw+8) != size - (u32)bytes) goto bad;
    return csw[12] ? -2 : bytes;
bad:
    recover(d);
    return -1;
}
static int enumerate(hc_t *h, int port, usb_disk_t *d) {
    if (h->dead) return 0;
    if (h->kind) {
        if (!ehci_reset_port(h,port)) return 0;
    } else {
        u16 reg = h->io + 0x10 + 2 * port;
        if (!(inw(reg) & 1) || (inw(reg) & 0x100)) return 0;
        outw(reg, 0x200); if (!frames_wait(h, 50)) return 0;
        outw(reg, 0); if (!frames_wait(h, 10)) return 0;
        outw(reg, 4); if (!frames_wait(h, 25) || (inw(reg) & 5) != 5) return 0;
    }
    memset(d, 0, sizeof(*d)); d->hc = h; d->port = port; d->ep0 = h->kind ? 64 : 8;
    if (control(d, 0x80, 6, 0x100, 0, reply, 8) != 8 || reply[1] != 1) return 0;
    u8 mps = reply[7];
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) return 0;
    if (h->kind && mps != 64) return 0;
    d->ep0 = mps;
    if (control(d, 0, 5, port + 1, 0, 0, 0) < 0 || !frames_wait(h, 3)) return 0;
    d->address = port + 1;
    if (control(d, 0x80, 6, 0x200, 0, config, 9) != 9 || config[1] != 2) return 0;
    unsigned len = config[2] | ((unsigned)config[3] << 8);
    if (len < 9 || len > sizeof(config) || config[0] < 9) return 0;
    if (control(d, 0x80, 6, 0x200, 0, config, len) != (int)len) return 0;
    int matching = 0, found = 0;
    for (unsigned pos = 0; pos + 2 <= len;) {
        unsigned n = config[pos], type = config[pos + 1];
        if (n < 2 || n > len - pos) return 0;
        const u8 *p = config + pos;
        if (type == 4) {
            if (matching && d->in && d->out) { found = 1; break; }
            d->in = d->out = 0;
            matching = n >= 9 && p[3] == 0 && p[5] == 8 && p[6] == 6 && p[7] == 0x50;
            if (matching) d->iface = p[2];
        } else if (matching && type == 5 && n >= 7 && (p[3] & 3) == 2) {
            unsigned packet = p[4] | ((unsigned)p[5] << 8);
            if (h->kind ? packet != 512 : (packet != 8 && packet != 16 && packet != 32 && packet != 64)) return 0;
            if (!(p[2] & 15)) return 0;
            if (p[2] & 0x80) { d->in = p[2] & 15; d->in_mps = packet; }
            else { d->out = p[2] & 15; d->out_mps = packet; }
        }
        pos += n;
    }
    if (!found && !(matching && d->in && d->out)) return 0;
    if (control(d, 0, 9, config[5], 0, 0, 0) < 0) return 0;
    u8 cdb[16]; memset(cdb, 0, sizeof(cdb));
    for (int i = 0; i < 5; i++) {
        cdb[0] = 0; /* TEST UNIT READY; clear initial unit attention */
        if (command(d, cdb, 6, 0, 0) == 0) break;
        cdb[0] = 3; cdb[4] = 18;
        command(d, cdb, 6, reply, 18); cdb[4] = 0;
        if (!frames_wait(h, 50)) return 0;
    }
    memset(cdb, 0, sizeof(cdb)); cdb[0] = 0x25;
    if (command(d, cdb, 10, reply, 8) != 8 || be32(reply+4) != 512) return 0;
    u32 last = be32(reply);
    if (last == 0xFFFFFFFFu) return 0; /* needs READ CAPACITY(16) */
    d->sectors = last + 1;
    return d->sectors != 0;
}
static void init_controllers(void) {
    /* Claim high-speed port routing before starting companion UHCI hosts. */
    for (int pass = 0; pass < 2; pass++)
    for (u32 i = 0; i < pci_count() && hc_count < HC_MAX; i++) {
        pci_dev_t *p = pci_get(i);
        if (p->class_code != 0x0C || p->subclass != 3 || p->prog_if != (pass ? 0 : 0x20)) continue;
        if (!pass) {
            ehci_seen++;
            if (ehci_init(p,&hcs[hc_count])) { hc_count++; ehci_count++; }
            continue;
        }
        u32 bar = pci_read32(p->bus, p->slot, p->func, 0x20);
        if (!(bar & 1) || !(bar & 0xFFE0)) continue;
        u32 cmd = pci_read32(p->bus, p->slot, p->func, 4);
        pci_write32(p->bus, p->slot, p->func, 4, (cmd & 0xFFFF) | 5);
        pci_write32(p->bus, p->slot, p->func, 0xC0, 0x2000); /* disable legacy SMI */
        hc_t *h = &hcs[hc_count]; memset(h,0,sizeof(*h)); h->io = bar & 0xFFE0; h->ports = 2;
        outw(h->io, 0); outw(h->io + 4, 0); outw(h->io, 2);
        int limit = 100000;
        while ((inw(h->io) & 2) && --limit) __asm__ volatile("pause");
        if (!limit) continue;
        queues[hc_count].link = queues[hc_count].element = TERM;
        for (int j = 0; j < 1024; j++) frames[hc_count][j] = physical(&queues[hc_count]) | 2;
        outl(h->io + 8, physical(frames[hc_count]));
        outw(h->io + 6, 0); outb(h->io + 12, 64); outw(h->io + 2, 0x3F);
        outw(h->io, 0xC1); /* Run, configured, 64-byte max packets */
        if (frames_wait(h, 20)) { hc_count++; uhci_count++; }
        else outw(h->io, 0);
    }
}
void usb_rescan(void) {
    if (!initialised) { init_controllers(); initialised = 1; }
    disk_count = 0; memset(disks, 0, sizeof(disks));
    memset(cache, 0, sizeof(cache)); cache_next = 0;
    for (int i = 0; i < hc_count; i++) for (int p = 0; p < hcs[i].ports && disk_count < USB_MAX; p++) {
        if (enumerate(&hcs[i], p, &disks[disk_count])) disk_count++;
    }
}
int usb_count(void) { return disk_count; }
u32 usb_sectors(int disk) {
    if (disk < 0 || disk >= disk_count) return 0;
    usb_disk_t *d = &disks[disk];
    return !d->hc->dead && (port_status(d->hc,d->port) & 5) == 5 ? d->sectors : 0;
}
const char *usb_status(void) {
    static char info[80];
    if (ehci_seen && !ehci_count && !disk_count) return ehci_error;
    if (!hc_count) return ehci_seen ? ehci_error : "USB: no UHCI/EHCI (OHCI/xHCI unsupported)";
    ksnprintf(info,sizeof(info),"USB: EHCI %d, UHCI %d; disks %d",ehci_count,uhci_count,disk_count);
    return info;
}
int usb_read(int disk, u32 lba, u8 count, void *buf) {
    u32 sectors = usb_sectors(disk);
    if (!count || lba >= sectors || count > sectors - lba) return -1;
    usb_disk_t *d = &disks[disk];
    u8 cdb[10];
    while (count) {
        int hit = -1;
        for (int i = 0; i < 32; i++)
            if (cache[i].valid && cache[i].disk == disk && cache[i].lba == lba) { hit = i; break; }
        if (hit >= 0) {
            memcpy(buf, cache[hit].data, 512);
            buf = (u8 *)buf + 512; lba++; count--; continue;
        }
        unsigned n = count > 8 ? 8 : count;
        memset(cdb, 0, sizeof(cdb)); cdb[0] = 0x28;
        cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
        cdb[8] = n;
        if (command(d, cdb, 10, buf, n * 512) != (int)n * 512) return -1;
        for (unsigned i = 0; i < n; i++) {
            unsigned slot = cache_next++ % 32;
            memcpy(cache[slot].data, (u8 *)buf + i * 512, 512);
            cache[slot].lba = lba + i; cache[slot].disk = disk; cache[slot].valid = 1;
        }
        buf = (u8 *)buf + n * 512; lba += n; count -= n;
    }
    return 0;
}
