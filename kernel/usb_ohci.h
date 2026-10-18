/* OHCI 1.0, full-speed root-port BOT. One synchronous ED + real/dummy TD.
 * DMA objects are PRIVATE per attempted controller, never shared with EHCI/UHCI.
 * Faults fence until reboot; even a failed stop cannot recycle late-DMA memory.
 * No interrupts, periodic/isochronous traffic, hubs or low-speed storage. */
typedef struct { u32 flags,tail; volatile u32 head; u32 next; } ohci_ed_t;
typedef struct { volatile u32 flags,cbp; u32 next,end; } ohci_td_t;
typedef struct {
    u32 interrupt[32]; volatile u16 frame; u16 pad; volatile u32 done; u8 reserved[120];
} ohci_hcca_t;
typedef struct {
    ohci_hcca_t hcca __attribute__((aligned(256)));
    ohci_ed_t ed __attribute__((aligned(16)));
    ohci_td_t td[2] __attribute__((aligned(16)));
    u8 data[4096] __attribute__((aligned(4096)));
} ohci_dma_t;
_Static_assert(sizeof(ohci_ed_t)==16 && sizeof(ohci_td_t)==16,"OHCI ED/TD");
_Static_assert(sizeof(ohci_hcca_t)==256,"OHCI HCCA");
static ohci_dma_t ohci_pool[HC_MAX];
static unsigned ohci_used;
static int ohci_count,ohci_seen;
static const char *ohci_error="USB: OHCI unavailable";
#ifndef OHCI_RD
#define OHCI_RD(h,o) ((h)->op[(o)/4])
#define OHCI_WR(h,o,v) ((h)->op[(o)/4]=(v))
#define OHCI_PAUSE() __asm__ volatile("pause")
#define OHCI_MMIO(base) ((volatile u32 *)(kv_addr_t)(base))
#endif
static int ohci_dma_address(const void *p,u32 size,u32 alignment,u32 *out) {
    kv_addr_t a=(kv_addr_t)p;
    if(!size || a>0xFFFFFFFFu || size-1>0xFFFFFFFFu-a) return 0;
    u32 base=physical(p);
    if(!base || (base&(alignment-1)) || size-1>0xFFFFFFFFu-base) return 0;
    for(u32 off=0;off<size;) {
        if(physical((const u8 *)p+off)!=base+off) return 0;
        u32 n=4096-(((u32)a+off)&4095);off+=n<size-off?n:size-off;
    }
    *out=base;return 1;
}
#ifndef OHCI_DMA
#define OHCI_DMA ohci_dma_address
#endif
static int ohci_wait(hc_t *h,unsigned reg,u32 mask,u32 value) {
    u64 end=clock_now()+1000000000ULL;
    for(unsigned n=1000000;n && clock_now()<end;n--) {
        if((OHCI_RD(h,reg)&mask)==value) return 1;
        OHCI_PAUSE();
    }
    return 0;
}
static int ohci_fence(hc_t *h) {
    h->dead=1;
    OHCI_WR(h,0x14,0xFFFFFFFFu);OHCI_WR(h,4,0); /* disable IRQs/lists, USB reset */
    (void)OHCI_RD(h,4);
    u32 cmd=pci_read32(h->bus,h->slot,h->func,4)&0xFFFF;
    pci_write32(h->bus,h->slot,h->func,4,cmd&~4u);
    (void)pci_read32(h->bus,h->slot,h->func,4);
    return -1;
}
static int ohci_run(usb_disk_t *d,u8 pid,u8 ep,u16 mps,int *toggle,void *buf,unsigned len) {
    hc_t *h=d->hc;
    if(h->dead || h->pool>=ohci_used || !toggle || (mps!=8 && mps!=16 && mps!=32 && mps!=64) || ep>15 || d->address>127 || len>4096 || (len && !buf) ||
       (pid!=IN && pid!=OUT && pid!=SETUP)) return -1;
    u32 port=OHCI_RD(h,0x54+4*d->port);
    if((port&3)!=3 || (d->generation && (port&0x1F0000)) || (OHCI_RD(h,0x0C)&16)) return ohci_fence(h);
    ohci_dma_t *s=&ohci_pool[h->pool];
    u32 ed,td,data;
    if(!OHCI_DMA(&s->ed,sizeof(s->ed),16,&ed) || !OHCI_DMA(s->td,sizeof(s->td),16,&td) ||
       !OHCI_DMA(s->data,sizeof(s->data),4096,&data)) return ohci_fence(h);
    /* Prior transfer retired two frames before returning. No live ED/TD edits. */
    memset(&s->ed,0,sizeof(s->ed));memset(s->td,0,sizeof(s->td));
    if(pid!=IN && len) memcpy(s->data,buf,len);
    s->td[0].flags=0xF0000000u | ((u32)(2+(*toggle&1))<<24) |
                   ((pid==IN?2u:pid==OUT?1u:0u)<<19) | (pid==IN?1u<<18:0);
    s->td[0].cbp=len?data:0;s->td[0].end=len?data+len-1:0;s->td[0].next=td+16;
    s->ed.flags=d->address | ((u32)ep<<7) | ((u32)mps<<16);
    s->ed.tail=td+16;s->ed.head=td;
    unsigned head=ep?0x28:0x20,enable=ep?32:16,kick=ep?4:2;
    OHCI_WR(h,0x0C,2); /* only WDH; never erase UE/link evidence */
    __sync_synchronize();
    port=OHCI_RD(h,0x54+4*d->port);
    if((port&3)!=3 || (d->generation && (port&0x1F0000)) || (OHCI_RD(h,0x0C)&16)) return ohci_fence(h);
    OHCI_WR(h,head,ed);OHCI_WR(h,4,OHCI_RD(h,4)|enable);OHCI_WR(h,8,kick);
    u32 start=OHCI_RD(h,0x3C)&2047;
    u64 end=clock_now()+1000000000ULL;
    int done=0;
    for(unsigned n=2000000;n && clock_now()<end;n--) {
        port=OHCI_RD(h,0x54+4*d->port);
        if((port&3)!=3 || (d->generation && (port&0x1F0000)) || (OHCI_RD(h,0x0C)&16)) break;
        /* CC may already be NoError after an intermediate OUT packet.
         * Only ED retirement (or halt), NOT CC alone, completes the TD. */
        u32 head_value=s->ed.head;
        if((head_value&1) || ((head_value&~15u)==td+16 && (s->td[0].flags>>28)!=15)) { done=1;break; }
        if(((OHCI_RD(h,0x3C)-start)&2047)>=250) break;
        OHCI_PAUSE();
    }
    __sync_synchronize();
    unsigned cc=s->td[0].flags>>28;
    if(!done || cc || (s->ed.head&1) || (s->ed.head&~15u)!=td+16) return ohci_fence(h);
    u32 cbp=s->td[0].cbp;
    if(cbp && (!len || cbp<data || cbp>=data+len)) return ohci_fence(h);
    unsigned actual=cbp?cbp-data:len;
    if(pid!=IN && actual!=len) return ohci_fence(h);
    OHCI_WR(h,4,OHCI_RD(h,4)&~enable);s->ed.flags|=1u<<14;
    __sync_synchronize();
    if(!frames_wait(h,2) || (OHCI_RD(h,0x0C)&16)) return ohci_fence(h);
    OHCI_WR(h,head,0); /* DMA retirement complete; next transfer may reuse */
    *toggle=(s->ed.head>>1)&1;
    if(pid==IN && actual) memcpy(buf,s->data,actual);
    return (int)actual;
}
static int ohci_transfer(usb_disk_t *d,u8 pid,u8 ep,u16 mps,int *toggle,void *buf,unsigned len) {
    hc_t *h=d->hc;
    if(h->dead || __sync_lock_test_and_set(&h->busy,1)) return -1;
    int result=ohci_run(d,pid,ep,mps,toggle,buf,len);
    __sync_lock_release(&h->busy);return result;
}
static int ohci_reset_port(hc_t *h,int port) {
    unsigned reg=0x54+4*port;u32 status=OHCI_RD(h,reg);
    if(h->dead || !(status&1) || (status&(1u<<9))) return 0; /* LS not BOT */
    OHCI_WR(h,reg,1u<<8); /* power on, NOT a read/modify/write W1C register */
    if(!frames_wait(h,20)) { ohci_fence(h);return 0; }
    OHCI_WR(h,reg,1u<<4);
    if(!ohci_wait(h,reg,1u<<4,0) || !frames_wait(h,10)) { ohci_fence(h);return 0; }
    status=OHCI_RD(h,reg);
    return (status&3)==3 && !(status&(1u<<9));
}
static int ohci_fail(const char *why) { ohci_error=why;return 0; }
static int ohci_init(pci_dev_t *p,hc_t *h) {
    u32 bar=pci_read32(p->bus,p->slot,p->func,0x10),base=bar&~15u;
    if((bar&7) || base<0x10000 || base>0xFFFFF000u || ohci_used>=HC_MAX) return ohci_fail("USB: invalid OHCI BAR/pool full");
    if(paging_map_mmio(base,4096)<0) return ohci_fail("USB: OHCI MMIO map failed");
    memset(h,0,sizeof(*h));h->op=OHCI_MMIO(base);h->kind=2;
    h->bus=p->bus;h->slot=p->slot;h->func=p->func;
    u32 cmd=pci_read32(p->bus,p->slot,p->func,4)&0xFFFF;
    pci_write32(p->bus,p->slot,p->func,4,cmd|2);
    if((OHCI_RD(h,0)&255)!=0x10) return ohci_fail("USB: unsupported OHCI revision");
    if(OHCI_RD(h,4)&256) {
        OHCI_WR(h,8,8); /* ownership request; don't fight firmware on failure */
        if(!ohci_wait(h,4,256,0)) return ohci_fail("USB: OHCI firmware handoff failed");
    }
    /* Firmware relinquished. Stop bus mastering BEFORE replacing its lists. */
    pci_write32(p->bus,p->slot,p->func,4,(cmd|2)&~4u);
    (void)pci_read32(p->bus,p->slot,p->func,4);
    OHCI_WR(h,0x14,0xFFFFFFFFu);OHCI_WR(h,4,0);OHCI_WR(h,8,1);
    if(!ohci_wait(h,8,1,0)) return ohci_fail("USB: OHCI reset timed out");
    unsigned ports=OHCI_RD(h,0x48)&255;
    if(!ports || ports>15) return ohci_fail("USB: invalid OHCI root hub");
    h->ports=(u8)ports;h->pool=(u8)ohci_used++;
    ohci_dma_t *s=&ohci_pool[h->pool];u32 hcca;
    if(!OHCI_DMA(&s->hcca,sizeof(s->hcca),256,&hcca)) return ohci_fail("USB: OHCI DMA unavailable");
    memset(s,0,sizeof(*s));
    OHCI_WR(h,0x14,0xFFFFFFFFu);OHCI_WR(h,0x0C,0xFFFFFFFFu);
    OHCI_WR(h,0x18,hcca);
    OHCI_WR(h,0x20,0);OHCI_WR(h,0x24,0);OHCI_WR(h,0x28,0);OHCI_WR(h,0x2C,0);
    OHCI_WR(h,0x34,11999u|(((11999u-210)*6/7)<<16)|((OHCI_RD(h,0x34)^(1u<<31))&(1u<<31)));
    OHCI_WR(h,0x40,11999*9/10);OHCI_WR(h,0x44,0x628);
    __sync_synchronize();
    pci_write32(p->bus,p->slot,p->func,4,(cmd|6));
    OHCI_WR(h,4,0x83); /* operational, CBSR=3; no lists/IRQs until a transfer */
    OHCI_WR(h,0x50,1u<<16); /* global power on */
    for(unsigned i=0;i<ports;i++) OHCI_WR(h,0x54+4*i,1u<<8);
    if(!frames_wait(h,20+2*(OHCI_RD(h,0x48)>>24))) {
        ohci_fence(h);return ohci_fail("USB: OHCI frame clock stopped");
    }
    return 1;
}
