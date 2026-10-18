/* UHCI/OHCI (full speed), EHCI (high speed), BOT/SCSI storage; writes locked by default.
 * Bounded hubs + experimental EHCI FS async TT; no LS/periodic splits, xHCI/UAS/4Kn. DMA is static,
 * below 4 GiB, packetised at endpoint MPS; no IRQ or timer dependency. */
#include "kernel.h"
#include "usb_bot.h"

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
    u8 kind, ports, dead, bus, slot, func, pool; /* 0 UHCI, 1 EHCI, 2 OHCI */
    u32 busy; /* EHCI/OHCI per-controller synchronous DMA guard */
} hc_t;
typedef struct {
    hc_t *hc;
    u32 sectors, write_start, write_count, generation;
#if KV_STORAGE_LAB
    u32 journal_start,journal_count;
#endif
    u8 write_allowed, sense_key, asc, ascq;
    u16 in_mps, out_mps;
    u8 address, port, ep0, in, out, itoggle, otoggle, iface;
    u8 parent, downport, route_dead; /* root port stays in port for HCD guards */
    u8 speed; /* EHCI encoding: 0 full, 2 high; consumes former trailing padding */
} usb_disk_t;
static hc_t hcs[HC_MAX];
static usb_disk_t disks[USB_MAX];
static int hc_count, disk_count, initialised, ehci_count, uhci_count, ehci_seen;
static u32 frames[HC_MAX][1024] __attribute__((aligned(4096)));
static qh_t queues[HC_MAX] __attribute__((aligned(16)));
static u8 setup_buf[8] __attribute__((aligned(16)));
static u8 config[1024], reply[64];
static u32 next_tag,enumeration_generation;
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
static int usb_tt_route(usb_disk_t *d,u32 *caps);
#include "usb_ehci.h"
#include "usb_ohci.h"
static unsigned frame_number(hc_t *h) {
    return h->kind==2 ? OHCI_RD(h,0x3C)&2047 : h->kind==1 ? (ehci_reg(h, 0x0C) >> 3) & 2047 : inw(h->io + 6) & 2047;
}
static u32 port_status(hc_t *h, int p) {
    if(h->kind==2) {
        u32 r=OHCI_RD(h,0x54+4*p);
        return (r&1)|((r&2)<<1)|((r&0x1F0000)?2u:0);
    }
    return h->kind==1 ? ehci_reg(h, 0x44 + 4*p) : inw(h->io + 0x10 + 2*p);
}
/* Both frame counters wrap at 2048 milliseconds. TSC bounds a stopped HC. */
static int frames_wait(hc_t *h, unsigned n) {
    if(h->kind==1) return ehci_frames_wait(h,n);
    unsigned start = frame_number(h);
    u64 until = clock_now() + 1000000000ULL;
    while (((frame_number(h) - start) & 2047u) < n) {
        if (h->dead || (h->kind==2 ? OHCI_RD(h,0x0C)&16 : h->kind==1 ? ehci_reg(h,4) & 0x1000 : inw(h->io+2) & 0x20) || clock_now() > until) return 0;
        __asm__ volatile("pause");
    }
    return 1;
}
/* Quarantine a controller if a link cannot be isolated or DMA retired.
 * UHCI runtime-failed slots and their private DMA are never reused. */
static void usb_stop(hc_t *h) {
    if(h->kind==2) { ohci_fence(h);return; }
    if(h->kind==1) { ehci_fence(h);return; }
    h->dead=1;
    queues[h-hcs].element=TERM;outw(h->io,0);
    u32 cmd=pci_read32(h->bus,h->slot,h->func,4)&0xFFFF;
    pci_write32(h->bus,h->slot,h->func,4,cmd&~4u);
    (void)pci_read32(h->bus,h->slot,h->func,4);
}
static int usb_route_live(usb_disk_t *d);
#include "usb_uhci.h"
static int transfer(usb_disk_t *d,u8 pid,u8 ep,u16 mps,
                    int *toggle,void *buf,unsigned len) {
    if(d->hc->kind==2) return ohci_transfer(d,pid,ep,mps,toggle,buf,len);
    if(d->hc->kind==1) return ehci_transfer(d,pid,ep,mps,toggle,buf,len);
    return uhci_transfer(d,pid,ep,mps,toggle,buf,len);
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
static u32 be32(const u8 *p) { return p[3] | ((u32)p[2]<<8) | ((u32)p[1]<<16) | ((u32)p[0]<<24); }
static void recover(usb_disk_t *d) {
    control(d, 0x21, 0xFF, 0, d->iface, 0, 0); /* BOT reset */
    control(d, 2, 1, 0, d->in | 0x80, 0, 0); /* clear halt */
    control(d, 2, 1, 0, d->out, 0, 0);
    d->itoggle = d->otoggle = 0;
}
/* Endpoint toggles remain transport state, not unaligned integer casts. */
static int bot_transfer(void *ctx,int input,void *buf,unsigned size) {
    usb_disk_t *d=ctx; int toggle=input?d->itoggle:d->otoggle;
    int n=transfer(d,input?IN:OUT,input?d->in:d->out,input?d->in_mps:d->out_mps,&toggle,buf,size);
    if(n>=0) { if(input) d->itoggle=(u8)toggle; else d->otoggle=(u8)toggle; }
    return n;
}
static void bot_reset(void *ctx) { recover(ctx); }
static int command_dir(usb_disk_t *d,const u8 *cdb,u8 len,int input,void *buf,u32 size) {
    if(!usb_route_live(d)) return -1;
    usb_bot_io_t io={d,bot_transfer,bot_reset};
    int n=usb_bot_command(&io,++next_tag,cdb,len,input,buf,size);
    return usb_route_live(d)?n:-1;
}
static int command(usb_disk_t *d,const u8 *cdb,u8 len,void *buf,u32 size) {
    return command_dir(d,cdb,len,1,buf,size);
}
static void invalidate(int disk) {
    for(unsigned i=0;i<32;i++) if(cache[i].disk==disk) cache[i].valid=0;
}
/* Best-effort diagnosis only: never replay WRITE after an uncertain outcome. */
static int write_failed(int disk) {
    usb_disk_t *d=&disks[disk]; d->write_allowed=0; invalidate(disk);
    d->sense_key=d->asc=d->ascq=0;
    u8 cdb[6]={3,0,0,0,18,0},sense[18];
    int n=command(d,cdb,6,sense,sizeof(sense));
    if(n>=14 && ((sense[0]&0x7f)==0x70 || (sense[0]&0x7f)==0x71)) {
        d->sense_key=sense[2]&15; d->asc=sense[12]; d->ascq=sense[13];
    } else if(n>=8 && ((sense[0]&0x7f)==0x72 || (sense[0]&0x7f)==0x73)) {
        d->sense_key=sense[1]&15; d->asc=sense[2]; d->ascq=sense[3];
    }
    return -1;
}
#include "usb_hub.h"
/* Do not preserve old address-zero responders between enumeration attempts. */
static void root_disable(hc_t *h,int p) {
    if(h->dead) return;
    if(h->kind==2) OHCI_WR(h,0x54+4*p,1); /* ClearPortEnable, no W1C */
    else if(h->kind==1) ehci_port(h,p,0,4);
    else outw(h->io+0x10+2*p,(u16)(port_status(h,p)&~0xEu));
    if(port_status(h,p)&4) usb_stop(h);
}
static int root_reset(hc_t *h,int port) {
    if(h->dead) return 0;
    if(h->kind==2) { if(!ohci_reset_port(h,port)) return 0; }
    else if(h->kind==1) { if(!ehci_reset_port(h,port)) return 0; }
    else {
        u16 reg=h->io+0x10+2*port;
        if(!(inw(reg)&1) || (inw(reg)&0x100)) return 0;
        outw(reg,0x200);if(!frames_wait(h,50)) return 0;
        outw(reg,0);if(!frames_wait(h,10)) return 0;
        outw(reg,4);if(!frames_wait(h,25) || (inw(reg)&5)!=5) return 0;
    }
    /* Acknowledge the reset here, NEVER after enumeration or during live I/O. */
    if(h->kind==2) OHCI_WR(h,0x54+4*port,0x1F0000);
    else if(h->kind==1) ehci_port(h,port,0x2A,0);
    else outw(h->io+0x10+2*port,(u16)(port_status(h,port)|0xA));
    return 1;
}
static void init_controllers(void) {
    /* Claim high-speed port routing before starting companion UHCI hosts. */
    for (int pass = 0; pass < 2; pass++)
    for (u32 i = 0; i < pci_count() && hc_count < HC_MAX; i++) {
        pci_dev_t *p = pci_get(i);
        if (p->class_code != 0x0C || p->subclass != 3 || (!pass ? p->prog_if!=0x20 : (p->prog_if!=0 && p->prog_if!=0x10))) continue;
        if (!pass) {
            ehci_seen++;
            if (ehci_init(p,&hcs[hc_count])) { hc_count++; ehci_count++; }
            continue;
        }
        if(p->prog_if==0x10) {
            ohci_seen++;
            if(ohci_init(p,&hcs[hc_count])) { hc_count++;ohci_count++; }
            continue;
        }
        u32 bar = pci_read32(p->bus, p->slot, p->func, 0x20);
        if (!(bar & 1) || !(bar & 0xFFE0)) continue;
        u32 cmd = pci_read32(p->bus, p->slot, p->func, 4);
        pci_write32(p->bus, p->slot, p->func, 4, (cmd & 0xFFFF) | 5);
        pci_write32(p->bus, p->slot, p->func, 0xC0, 0x2000); /* disable legacy SMI */
        hc_t *h = &hcs[hc_count]; memset(h,0,sizeof(*h)); h->io = bar & 0xFFE0; h->ports = 2;
        h->bus=p->bus;h->slot=p->slot;h->func=p->func;
        outw(h->io, 0); outw(h->io + 4, 0); outw(h->io, 2);
        int limit = 100000;
        while ((inw(h->io) & 2) && --limit) __asm__ volatile("pause");
        if (!limit) { usb_stop(h);continue; }
        queues[hc_count].link = queues[hc_count].element = TERM;
        for (int j = 0; j < 1024; j++) frames[hc_count][j] = physical(&queues[hc_count]) | 2;
        outl(h->io + 8, physical(frames[hc_count]));
        outw(h->io + 6, 0); outb(h->io + 12, 64); outw(h->io + 2, 0x3F);
        outw(h->io, 0xC1); /* Run, configured, 64-byte max packets */
        if (frames_wait(h, 20)) { hc_count++; uhci_count++; }
        else usb_stop(h);
    }
}
void usb_rescan(void) {
    if(!++enumeration_generation) ++enumeration_generation;
    if (!initialised) { init_controllers(); initialised = 1; }
    disk_count = 0; memset(disks, 0, sizeof(disks));
    memset(cache, 0, sizeof(cache)); cache_next = 0;
    memset(hubs,0,sizeof(hubs));hub_count=hub_refused=0;
    /* Global isolation pass BEFORE reusing any descendant address. Root
     * addresses are reserved; every accepted downstream reset disables its
     * children before the recursive allocator may reach their old addresses. */
    for(int i=0;i<hc_count;i++) {
        next_address[i]=hcs[i].ports+1;
        for(int p=0;p<hcs[i].ports;p++) root_disable(&hcs[i],p);
    }
    for(int i=0;i<hc_count;i++) for(int p=0;p<hcs[i].ports && disk_count<USB_MAX;p++) {
        hc_t *h=&hcs[i];
        usb_disk_t d;memset(&d,0,sizeof(d));d.hc=h;d.port=p;d.speed=h->kind==1?2:0;d.ep0=d.speed==2?64:8;
        if(!root_reset(h,p) || !enumerate_device(&d,p+1,0)) root_disable(h,p);
    }
}
int usb_count(void) { return disk_count; }
/* Capacity/epoch queries are non-I/O: inspect known edge blocks and root
 * latches. Actual BOT, cached reads and lease grants actively poll the route. */
u32 usb_sectors(int disk) {
    if (disk < 0 || disk >= disk_count) return 0;
    usb_disk_t *d = &disks[disk];
    if(d->generation==enumeration_generation && usb_route_check(d,0)) return d->sectors;
    d->write_allowed=0;invalidate(disk);return 0;
}
u32 usb_generation(int disk) { return usb_sectors(disk)?disks[disk].generation:0; }
const char *usb_status(void) {
    static char info[128];
    if (ehci_seen && !ehci_count && !disk_count) return ehci_error;
    if (!hc_count) return ohci_seen ? ohci_error : ehci_seen ? ehci_error : "USB: no UHCI/OHCI/EHCI (xHCI unsupported)";
    unsigned hs=0;for(unsigned i=0;i<hub_count;i++) hs+=hubs[i].dev.hc->kind==1;
    ksnprintf(info,sizeof(info),"USB: EHCI %d, UHCI %d, OHCI %d; disks %d; hubs %u (HS %u), refused %u",ehci_count,uhci_count,ohci_count,disk_count,hub_count,hs,hub_refused);
    return info;
}
int usb_read(int disk, u32 lba, u8 count, void *buf) {
    u32 sectors = usb_sectors(disk);
    if (!buf || !count || lba >= sectors || count > sectors - lba) return -1;
    usb_disk_t *d = &disks[disk];
    u8 cdb[10];int cached=0;
    while (count) {
        int hit = -1;
        for (int i = 0; i < 32; i++)
            if (cache[i].valid && cache[i].disk == disk && cache[i].lba == lba) { hit = i; break; }
        if (hit >= 0) {
            if(!cached && !usb_route_live(d)) { invalidate(disk);return -1; }
            cached=1;
            memcpy(buf, cache[hit].data, 512);
            buf = (u8 *)buf + 512; lba++; count--; continue;
        }
        unsigned n = count > 8 ? 8 : count;
        cached=0;
        memset(cdb, 0, sizeof(cdb)); cdb[0] = 0x28;
        cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
        cdb[8] = n;
        if (command(d, cdb, 10, buf, n * 512) != (int)n * 512) {
            d->write_allowed=0; invalidate(disk); return -1;
        }
        for (unsigned i = 0; i < n; i++) {
            unsigned slot = cache_next++ % 32;
            memcpy(cache[slot].data, (u8 *)buf + i * 512, 512);
            cache[slot].lba = lba + i; cache[slot].disk = disk; cache[slot].valid = 1;
        }
        buf = (u8 *)buf + n * 512; lba += n; count -= n;
    }
    if(cached && !usb_route_live(d)) { invalidate(disk);return -1; }
    return 0;
}

/* Internal FAT lease: the caller has validated an owned WAL before asking.
 * Generation check prevents an old namespace authorizing a replacement stick.
 * Granted per operation/recovery and revoked before returning to the caller. */
void usb_fat_revoke(int disk) {
    if(disk>=0 && disk<disk_count) disks[disk].write_allowed=0;
}
int usb_fat_authorize(int disk,u32 start,u32 count,u32 generation) {
    usb_fat_revoke(disk);
    u32 sectors=usb_sectors(disk);
    if(!generation || generation!=usb_generation(disk) || !usb_route_live(&disks[disk]) || !count ||
       start>=sectors || count>sectors-start) return -1;
#if KV_STORAGE_LAB
    disks[disk].journal_count=0;
#endif
    disks[disk].write_start=start;disks[disk].write_count=count;
    disks[disk].write_allowed=1;return 0;
}

/* Internal lab grant is absent from normal kernels. A rescan/disconnect/failure
 * clears it; the interval is a disposable scratch extent, never a whole disk. */
#if KV_STORAGE_LAB
int usb_test_authorize(int disk,u32 start,u32 count) {
    u32 sectors=usb_sectors(disk);
    if(disk>=0 && disk<disk_count) disks[disk].write_allowed=0;
    if(!count || start>=sectors || count>sectors-start) return -1;
    disks[disk].journal_count=0;
    disks[disk].write_start=start; disks[disk].write_count=count;
    disks[disk].write_allowed=1; return 0;
}
/* Two disjoint, individually bounded extents; never grant their intervening
 * gap (which may contain GPT backup metadata or another partition). */
int usb_test_authorize_pair(int disk,u32 start,u32 count,u32 journal,u32 n) {
    u32 sectors=usb_sectors(disk);
    if(disk>=0 && disk<disk_count) disks[disk].write_allowed=0;
    if(!count || !n || start>=sectors || count>sectors-start ||
       journal>=sectors || n>sectors-journal || !(start+count<=journal || journal+n<=start)) return -1;
    if(usb_test_authorize(disk,start,count)<0) return -1;
    disks[disk].journal_start=journal;disks[disk].journal_count=n;return 0;
}
#endif
static int in_range(u32 lba,u32 count,u32 start,u32 n) {
    return lba>=start && lba-start<n && count<=n-(lba-start);
}
int usb_write(int disk,u32 lba,u8 count,const void *buf) {
    u32 sectors=usb_sectors(disk);
    if(!buf || !count || lba>=sectors || count>sectors-lba) return -1;
    usb_disk_t *d=&disks[disk];
    int allowed=in_range(lba,count,d->write_start,d->write_count);
#if KV_STORAGE_LAB
    allowed|=in_range(lba,count,d->journal_start,d->journal_count);
#endif
    if(!d->write_allowed || !allowed) return -1;
    invalidate(disk); /* Also invalidate on partial/failed writes. */
    while(count) {
        unsigned n=count>8?8:count;
        u8 cdb[10]={0x2A,0,0,0,0,0,0,0,0,0};
        cdb[2]=lba>>24; cdb[3]=lba>>16; cdb[4]=lba>>8; cdb[5]=lba; cdb[8]=(u8)n;
        if(command_dir(d,cdb,10,0,(void *)buf,n*512)!=(int)n*512) return write_failed(disk);
        buf=(const u8 *)buf+n*512; lba+=n; count-=(u8)n;
    }
    return 0;
}
int usb_sync(int disk) {
    if(!usb_sectors(disk) || !disks[disk].write_allowed) return -1;
    u8 cdb[10]={0x35,0,0,0,0,0,0,0,0,0}; /* SYNCHRONIZE CACHE(10), IMMED=0 */
    if(command_dir(&disks[disk],cdb,10,0,0,0)!=0) return write_failed(disk);
    return 0;
}

int usb_sense(int disk,u8 *key,u8 *asc,u8 *ascq) {
    if(disk<0 || disk>=disk_count) return -1;
    *key=disks[disk].sense_key; *asc=disks[disk].asc; *ascq=disks[disk].ascq;
    return 0;
}
