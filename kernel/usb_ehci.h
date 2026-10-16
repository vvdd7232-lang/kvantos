/* Private EHCI backend for usb.c: high-speed root ports, async QH/qTD.
 * Reuses BOT/SCSI, enumeration and the read-only cache, not another USB stack.
 * One synchronous transfer at a time. Stop ASS before touching DMA objects. */
typedef struct {
    u32 next, alt;
    volatile u32 token;
    u32 page[5], upper[5], pad[3];
} ehci_td_t;
typedef struct {
    u32 link, endpoint, caps, current;
    ehci_td_t overlay;
} __attribute__((aligned(32))) ehci_qh_t;
static const char *ehci_error="USB: EHCI not initialized";
static int ehci_fail(const char *why) { ehci_error=why; return 0; }
_Static_assert(__builtin_offsetof(ehci_qh_t,overlay)==16,"EHCI QH overlay offset");
_Static_assert(sizeof(ehci_td_t)==64,"EHCI qTD allocation size");
static ehci_td_t ehci_td __attribute__((aligned(32)));
static ehci_qh_t ehci_qh[HC_MAX];

static u32 ehci_reg(hc_t *h, unsigned off) { return h->op[off/4]; }
static void ehci_write(hc_t *h, unsigned off, u32 value) {
    h->op[off/4]=value;
    (void)h->op[off/4]; /* flush posted register writes */
}
static int ehci_wait(hc_t *h, unsigned reg, u32 mask, u32 value) {
    u64 until=clock_now()+1000000000ULL;
    do {
        if ((ehci_reg(h,reg)&mask)==value) return 1;
        __asm__ volatile("pause");
    } while (clock_now()<until);
    return 0;
}
static int ehci_async_off(hc_t *h) {
    ehci_write(h,0,ehci_reg(h,0)&~32u);
    if (ehci_wait(h,4,0x8000,0)) return 1;
    /* Failed hardware must not keep DMAing into reusable buffers. */
    ehci_write(h,0,0);
    ehci_wait(h,4,0x1000,0x1000);
    u32 cmd=pci_read32(h->bus,h->slot,h->func,4)&0xFFFF;
    pci_write32(h->bus,h->slot,h->func,4,cmd&~4u);
    (void)pci_read32(h->bus,h->slot,h->func,4);
    h->dead=1;
    return 0;
}
static int ehci_transfer(usb_disk_t *d, u8 pid, u8 ep, u16 mps,
                         int *toggle, void *buf, unsigned len) {
    hc_t *h=d->hc;
    if (!ehci_async_off(h)) return -1;
    ehci_qh_t *q=&ehci_qh[h-hcs];
    memset(q,0,sizeof(*q)); memset(&ehci_td,0,sizeof(ehci_td));
    if (pid!=IN && len) memcpy(dma_buf,buf,len);
    ehci_td.next=ehci_td.alt=TERM;
    ehci_td.page[0]=physical(dma_buf); /* aligned page, <=4096 bytes */
    ehci_td.token=((u32)*toggle<<31) | (len<<16) | (3u<<10) |
                  ((pid==IN?1u:pid==SETUP?2u:0u)<<8) | 0x80;
    q->link=physical(q)|2;
    q->endpoint=d->address | ((u32)ep<<8) | (2u<<12) | (1u<<14) |
                (1u<<15) | ((u32)mps<<16) | (8u<<28);
    q->caps=1u<<30;
    q->overlay.next=physical(&ehci_td); q->overlay.alt=TERM;
    __asm__ volatile("" ::: "memory");
    ehci_write(h,0x18,physical(q));
    ehci_write(h,0,ehci_reg(h,0)|32);
    u32 start=(ehci_reg(h,0xC)>>3)&2047, status;
    u64 until=clock_now()+1000000000ULL;
    int result=-1;
    do {
        status=ehci_td.token;
        if (!(status&0x80)) {
            unsigned remaining=(status>>16)&0x7FFF;
            if (!(status&0x7C) && remaining<=len) result=(int)(len-remaining);
            break;
        }
        if (!(ehci_reg(h,0x44+4*d->port)&1) || (ehci_reg(h,4)&0x1010)) break;
        __asm__ volatile("pause");
    } while ((((ehci_reg(h,0xC)>>3)-start)&2047u)<250 && clock_now()<until);
    if (!ehci_async_off(h)) return -1;
    if (result>=0) {
        unsigned packets=(unsigned)result<len ? (unsigned)result/mps+1 : len?(len+mps-1)/mps:1;
        *toggle^=packets&1;
        if (pid==IN && result) memcpy(buf,dma_buf,(u32)result);
    }
    return result;
}
static void ehci_port(hc_t *h, int p, u32 set, u32 clear) {
    unsigned reg=0x44+4*p;
    ehci_write(h,reg,(ehci_reg(h,reg)&~(0x2Au|clear))|set); /* W1C changes */
}
static int ehci_reset_port(hc_t *h, int p) {
    if (!(ehci_reg(h,0x44+4*p)&1)) return 0;
    ehci_port(h,p,1u<<12,1u<<13); /* power, take port from companion */
    if (!frames_wait(h,20)) return 0;
    ehci_port(h,p,1u<<8,4); /* reset asserted, enable cleared */
    if (!frames_wait(h,50)) return 0;
    ehci_port(h,p,0,1u<<8);
    if (!ehci_wait(h,0x44+4*p,1u<<8,0) || !frames_wait(h,20)) return 0;
    if ((ehci_reg(h,0x44+4*p)&5)==5) return 1;
    /* FS/LS require a companion controller; split transactions/hubs absent. */
    ehci_port(h,p,1u<<13,0);
    return 0;
}
static int ehci_ownership(pci_dev_t *p, u32 caps) {
    unsigned at=(caps>>8)&255;
    for (int n=0; at && n<32; n++) {
        if (at<0x40 || at>0xF8 || (at&3)) return 0;
        u32 leg=pci_read32(p->bus,p->slot,p->func,at);
        if ((leg&255)==1) {
            pci_write32(p->bus,p->slot,p->func,at,leg|(1u<<24));
            u64 until=clock_now()+10000000000ULL;
            while (pci_read32(p->bus,p->slot,p->func,at)&(1u<<16)) {
                if (clock_now()>until) return 0; /* don't fight firmware */
                __asm__ volatile("pause");
            }
            u32 ctl=pci_read32(p->bus,p->slot,p->func,at+4);
            pci_write32(p->bus,p->slot,p->func,at+4,ctl&0xFFFF0000u);
            return 1;
        }
        at=(leg>>8)&255;
    }
    return at==0;
}
static int ehci_init_fail(hc_t *h, const char *why) {
    ehci_write(h,0,0);
    ehci_wait(h,4,0x1000,0x1000);
    u32 cmd=pci_read32(h->bus,h->slot,h->func,4)&0xFFFF;
    pci_write32(h->bus,h->slot,h->func,4,cmd&~4u);
    (void)pci_read32(h->bus,h->slot,h->func,4);
    h->dead=1;
    return ehci_fail(why);
}
static int ehci_init(pci_dev_t *p, hc_t *h) {
    u32 bar=pci_read32(p->bus,p->slot,p->func,0x10);
    if ((bar&1) || (bar&6)==2 || !(bar&~15u)) return ehci_fail("USB: invalid EHCI BAR");
    if ((bar&6)==4 && pci_read32(p->bus,p->slot,p->func,0x14)) return ehci_fail("USB: EHCI BAR above 4 GiB");
    u32 base=bar&~15u;
    if (paging_map_mmio(base,4096)<0) return ehci_fail("USB: EHCI MMIO map failed");
    u32 cmd=pci_read32(p->bus,p->slot,p->func,4)&0xFFFF;
    pci_write32(p->bus,p->slot,p->func,4,cmd|6);
    volatile u32 *cap=(volatile u32 *)(kv_addr_t)base;
    unsigned len=cap[0]&255, ports=cap[1]&15;
    if (len<0x10 || len>0x80 || (len&3) || !ports) return ehci_fail("USB: invalid EHCI capabilities");
    if (!ehci_ownership(p,cap[2])) return ehci_fail("USB: EHCI firmware handoff failed");
    memset(h,0,sizeof(*h));
    h->kind=1; h->ports=ports; h->op=(volatile u32 *)(kv_addr_t)(base+len);
    h->bus=p->bus; h->slot=p->slot; h->func=p->func;
    ehci_write(h,8,0); ehci_write(h,0,0);
    if (!ehci_wait(h,4,0x1000,0x1000)) return ehci_init_fail(h,"USB: EHCI stop timed out");
    ehci_write(h,0,2);
    if (!ehci_wait(h,0,2,0)) return ehci_init_fail(h,"USB: EHCI reset timed out");
    ehci_write(h,8,0);
    if (cap[2]&1) ehci_write(h,0x10,0); /* 64-bit DMA segment = low 4 GiB */
    /* A terminated periodic list keeps FRINDEX advancing even in emulators
     * that stop their frame timer when BOTH schedules are disabled. No IRQs. */
    unsigned index=(unsigned)(h-hcs);
    for (int i=0;i<1024;i++) frames[index][i]=TERM;
    ehci_write(h,0x14,physical(frames[index]));
    ehci_write(h,4,0x3F); ehci_write(h,0,0x80011);
    if (!ehci_wait(h,4,0x1000,0) || !frames_wait(h,20)) return ehci_init_fail(h,"USB: EHCI start timed out");
    ehci_write(h,0x40,1); /* route root ports to EHCI */
    for (unsigned i=0;i<ports;i++) ehci_port(h,i,1u<<12,0);
    if (!frames_wait(h,100)) return ehci_init_fail(h,"USB: EHCI frames stopped");
    return 1;
}
