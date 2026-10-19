/* EHCI 1.0 HS and experimental FS-through-TT async control/BOT. One private, pinned DMA pool per
 * attempted controller; failed attempts NEVER donate memory to another HC.
 * ASS retirement precedes reuse/copyout. No LS/periodic splits or TT recovery. */
typedef struct {
    u32 next,alt;
    volatile u32 token;
    u32 page[5],upper[5],pad[3];
} ehci_td_t;
typedef struct {
    u32 link,endpoint,caps;
    volatile u32 current;
    ehci_td_t overlay;
} __attribute__((aligned(32))) ehci_qh_t;
typedef struct {
    u32 periodic[1024] __attribute__((aligned(4096)));
    ehci_qh_t qh;
    ehci_td_t td __attribute__((aligned(32)));
    u8 data[4096] __attribute__((aligned(4096)));
} ehci_dma_t;
_Static_assert(__builtin_offsetof(ehci_qh_t,overlay)==16,"EHCI QH overlay offset");
_Static_assert(sizeof(ehci_td_t)==64,"EHCI qTD allocation size");
static ehci_dma_t ehci_pool[HC_MAX];
/* Last failed transfer, retained before fencing; no console I/O in polling.
 * Fixed 96-byte debugger ABI on i386/x64. Times are raw TSC ticks, NOT ns. */
enum { EHCI_LINK=1,EHCI_TSC,EHCI_FRAMES,EHCI_BUDGET,EHCI_ASS,
       EHCI_TOKEN,EHCI_SPLIT,EHCI_WRITEBACK,EHCI_SHORT,EHCI_ADDRESS,EHCI_START };
typedef struct {
    u64 start_tsc,stop_tsc;
    u32 reason,retired,iterations,start_frame,stop_frame,pre_td,pre_overlay;
    u32 post_td,post_overlay,command,status,port,current,next,endpoint,caps;
    u32 address,generation,len,post_frame;
} ehci_fault_t;
_Static_assert(sizeof(ehci_fault_t)==96,"EHCI fault snapshot ABI");
static volatile ehci_fault_t ehci_faults[HC_MAX];
static unsigned ehci_used;
static const char *ehci_error="USB: EHCI not initialized";
#ifndef EHCI_RD
#define EHCI_RD(h,o) ((h)->op[(o)/4])
#define EHCI_WR(h,o,v) ((h)->op[(o)/4]=(v))
#define EHCI_MMIO(base) ((volatile u32 *)(kv_addr_t)(base))
#define EHCI_PAUSE() __asm__ volatile("pause")
#endif
static u32 ehci_reg(hc_t *h,unsigned off) { return EHCI_RD(h,off); }
static void ehci_write(hc_t *h,unsigned off,u32 v) {
    EHCI_WR(h,off,v);(void)EHCI_RD(h,off);
}
static int ehci_fail(const char *why) { ehci_error=why;return 0; }
static int ehci_dma_address(const void *p,u32 size,u32 alignment,u32 *out) {
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
#ifndef EHCI_DMA
#define EHCI_DMA ehci_dma_address
#endif
/* Optional one-line serial dump of the fault snapshot on a fenced
 * transfer (kernel build only; host harness keeps this a no-op). */
#ifndef EHCI_DIAG
#define EHCI_DIAG(...) ((void)0)
#endif
static int ehci_wait(hc_t *h,unsigned reg,u32 mask,u32 value) {
    u64 end=clock_now()+1000000000ULL;
    for(unsigned n=1000000;n && clock_now()<end;n--) {
        if((ehci_reg(h,reg)&mask)==value) return 1;
        EHCI_PAUSE();
    }
    return 0;
}
static int ehci_frames_wait(hc_t *h,unsigned frames) {
    unsigned start=(ehci_reg(h,0xC)>>3)&2047;
    u64 end=clock_now()+1000000000ULL;
    for(unsigned n=1000000;n && clock_now()<end;n--) {
        if(h->dead || (ehci_reg(h,4)&0x1010)) return 0;
        if((((ehci_reg(h,0xC)>>3)-start)&2047u)>=frames) return 1;
        EHCI_PAUSE();
    }
    return 0;
}
static int ehci_fence(hc_t *h) {
    h->dead=1;ehci_write(h,8,0);ehci_write(h,0,0);
    /* Even a stuck engine has no reusable DMA: all its addresses stay pinned. */
    u32 cmd=pci_read32(h->bus,h->slot,h->func,4)&0xFFFF;
    pci_write32(h->bus,h->slot,h->func,4,cmd&~4u);
    (void)pci_read32(h->bus,h->slot,h->func,4);
    return -1;
}
static int ehci_async_off(hc_t *h) {
    ehci_write(h,0,ehci_reg(h,0)&~32u);
    if(ehci_wait(h,4,0x8000,0)) return 1;
    ehci_fence(h);return 0;
}
static int ehci_link(usb_disk_t *d) {
    hc_t *h=d->hc;u32 port=ehci_reg(h,0x44+4*d->port);
    return !h->dead && (port&5)==5 && !(port&(1u<<13)) &&
        (!d->generation || !(port&0x2A)) && !(ehci_reg(h,4)&0x1010) && (ehci_reg(h,0)&1);
}
static int ehci_failed(usb_disk_t *d,ehci_fault_t *f,unsigned reason) {
    hc_t *h=d->hc;ehci_dma_t *s=&ehci_pool[h->pool];
    if(!f->reason) f->reason=reason;
    f->post_td=s->td.token;f->post_overlay=s->qh.overlay.token;
    f->command=ehci_reg(h,0);f->status=ehci_reg(h,4);f->port=ehci_reg(h,0x44+4*d->port);
    f->post_frame=(ehci_reg(h,0xC)>>3)&2047;f->current=s->qh.current;f->next=s->qh.overlay.next;
    f->endpoint=s->qh.endpoint;f->caps=s->qh.caps;f->address=d->address;f->generation=d->generation;
    ehci_faults[h->pool]=*f;
    EHCI_DIAG("[ehci] transfer fenced: reason %u len %u ret %u iter %u td %08x ovl %08x cur %08x next %08x cmd %08x sts %08x port %08x frame %u->%u\n",
        f->reason,f->len,f->retired,f->iterations,f->post_td,f->post_overlay,f->current,f->next,
        f->command,f->status,f->port,f->start_frame,f->stop_frame);
    return ehci_fence(h);
}
/* Completion poll bounds. All three stay finite so a hung controller is
 * fenced, but the window must cover emulator completion latency: QEMU's
 * usb-storage finishes WRITE via async block I/O, and on a loaded TCG
 * host the CSW can legitimately take far longer than 250 frames. The
 * old 250-frame/2M-iteration window fenced healthy controllers in CI. */
#define EHCI_POLL_FRAMES 2000u
#define EHCI_POLL_TSC    8000000000ULL
#define EHCI_POLL_ITERS  16000000u
static int ehci_run(usb_disk_t *d,u8 pid,u8 ep,u16 mps,int *toggle,void *buf,unsigned len) {
    hc_t *h=d->hc;u32 caps=1u<<30;
    if((d->speed!=0 && d->speed!=2) || (d->speed==0 && !usb_tt_route(d,&caps))) return -1;
    if(h->pool>=ehci_used || !toggle || (*toggle!=0 && *toggle!=1) || ep>15 || d->address>127 ||
       d->port>=h->ports || len>4096 || (len && !buf) || (d->speed==2?(ep?mps!=512:mps!=64):(mps!=8 && mps!=16 && mps!=32 && mps!=64)) ||
       (pid!=IN && pid!=OUT && pid!=SETUP) || (pid==SETUP && (ep || len!=8 || *toggle))) return -1;
    ehci_fault_t f={0};f.len=len;
    if(!ehci_link(d)) return ehci_failed(d,&f,EHCI_LINK);
    if(!ehci_async_off(h)) return ehci_failed(d,&f,EHCI_ASS);
    ehci_dma_t *s=&ehci_pool[h->pool];u32 qh,td,data;
    if(!EHCI_DMA(&s->qh,sizeof(s->qh),32,&qh) || !EHCI_DMA(&s->td,sizeof(s->td),32,&td) ||
       !EHCI_DMA(s->data,sizeof(s->data),4096,&data)) return ehci_failed(d,&f,EHCI_ADDRESS);
    memset(&s->qh,0,sizeof(s->qh));memset(&s->td,0,sizeof(s->td));
    if(pid!=IN && len) memcpy(s->data,buf,len);
    s->td.next=s->td.alt=TERM;s->td.page[0]=len?data:0;
    s->td.token=((u32)*toggle<<31)|(len<<16)|(3u<<10)|((pid==IN?1u:pid==SETUP?2u:0u)<<8)|0x80;
    s->qh.link=qh|2;
    s->qh.endpoint=d->address|((u32)ep<<8)|((u32)d->speed<<12)|(1u<<14)|(1u<<15)|((u32)mps<<16)|
        (d->speed==2?8u<<28:ep?0:1u<<27);
    /* Async S/C masks are zero. Hardware owns SS/CS sequencing (EHCI 4.12.1). */
    s->qh.caps=caps;s->qh.overlay.next=td;s->qh.overlay.alt=TERM;
    __sync_synchronize();
    ehci_write(h,0x18,qh);
    if(!ehci_link(d)) return ehci_failed(d,&f,EHCI_LINK); /* latch race before issuing DMA */
    ehci_write(h,0,ehci_reg(h,0)|32);
    if(!ehci_wait(h,4,0x8000,0x8000)) return ehci_failed(d,&f,EHCI_START);
    u32 start=(ehci_reg(h,0xC)>>3)&2047;
    f.start_tsc=clock_now();f.start_frame=start;
    u64 end=f.start_tsc+EHCI_POLL_TSC;int done=0;unsigned left=EHCI_POLL_ITERS;
    for(;left && clock_now()<end;left--) {
        if(!ehci_link(d)) { f.reason=EHCI_LINK;break; }
        if(!(s->td.token&0x80)) { done=1;break; }
        if((((ehci_reg(h,0xC)>>3)-start)&2047u)>=EHCI_POLL_FRAMES) { f.reason=EHCI_FRAMES;break; }
        EHCI_PAUSE();
    }
    f.stop_tsc=clock_now();f.stop_frame=(ehci_reg(h,0xC)>>3)&2047;
    f.iterations=EHCI_POLL_ITERS-left;f.pre_td=s->td.token;f.pre_overlay=s->qh.overlay.token;
    if(!done && !f.reason) f.reason=left?EHCI_TSC:EHCI_BUDGET;
    /* qTD writeback alone does not prove that the QH is no longer live. */
    f.retired=ehci_async_off(h);
    if(!f.retired) return ehci_failed(d,&f,EHCI_ASS);
    __sync_synchronize();
    u32 status=s->td.token,overlay=s->qh.overlay.token;
    unsigned remaining=(status>>16)&0x7FFF;
    if(!done || !ehci_link(d) || (status&0xFC) || (overlay&0xFC) || remaining>len ||
       (d->speed==0 && ((status|overlay)&2)) ||
       ((overlay>>16)&0x7FFF)!=remaining || s->qh.current!=td || !(s->qh.overlay.next&TERM))
        return ehci_failed(d,&f,!ehci_link(d)?EHCI_LINK:(status|overlay)&0xFC?EHCI_TOKEN:
            d->speed==0 && ((status|overlay)&2)?EHCI_SPLIT:EHCI_WRITEBACK);
    unsigned actual=len-remaining;
    if(pid!=IN && actual!=len) return ehci_failed(d,&f,EHCI_SHORT);
    ehci_write(h,0x18,0);
    unsigned packets=actual<len?actual/mps+1:len?(len+mps-1)/mps:1;
    *toggle^=packets&1;
    if(pid==IN && actual) memcpy(buf,s->data,actual);
    return (int)actual;
}
static int ehci_transfer(usb_disk_t *d,u8 pid,u8 ep,u16 mps,int *toggle,void *buf,unsigned len) {
    hc_t *h=d->hc;
    if(h->dead || __sync_lock_test_and_set(&h->busy,1)) return -1;
    int r=ehci_run(d,pid,ep,mps,toggle,buf,len);
    __sync_lock_release(&h->busy);return r;
}
static void ehci_port(hc_t *h,int p,u32 set,u32 clear) {
    unsigned reg=0x44+4*p;
    ehci_write(h,reg,(ehci_reg(h,reg)&~(0x2Au|clear))|set);
}
static int ehci_reset_port(hc_t *h,int p) {
    if(h->dead || p<0 || p>=h->ports || !(ehci_reg(h,0x44+4*p)&1)) return 0;
    ehci_port(h,p,1u<<12,1u<<13);
    if(!ehci_frames_wait(h,20)) return 0;
    ehci_port(h,p,1u<<8,4);
    if(!ehci_frames_wait(h,50)) return 0;
    ehci_port(h,p,0,1u<<8);
    if(!ehci_wait(h,0x44+4*p,1u<<8,0) || !ehci_frames_wait(h,20)) return 0;
    if((ehci_reg(h,0x44+4*p)&5)==5) return 1;
    ehci_port(h,p,1u<<13,0); /* FS/LS still require a companion, not fake TT. */
    return 0;
}
static int ehci_ownership(pci_dev_t *p,u32 caps) {
    unsigned at=(caps>>8)&255;
    for(int n=0;at && n<32;n++) {
        if(at<0x40 || at>0xF8 || (at&3)) return 0;
        u32 leg=pci_read32(p->bus,p->slot,p->func,at);
        if((leg&255)==1) {
            pci_write32(p->bus,p->slot,p->func,at,leg|(1u<<24));
            u64 end=clock_now()+10000000000ULL;unsigned left=1000000;
            while(pci_read32(p->bus,p->slot,p->func,at)&(1u<<16)) {
                if(!left-- || clock_now()>end) return 0;
                EHCI_PAUSE();
            }
            u32 ctl=pci_read32(p->bus,p->slot,p->func,at+4);
            pci_write32(p->bus,p->slot,p->func,at+4,ctl&0xFFFF0000u);return 1;
        }
        at=(leg>>8)&255;
    }
    return at==0;
}
static int ehci_init_fail(hc_t *h,const char *why) {
    ehci_fence(h);return ehci_fail(why);
}
static int ehci_init_pool(pci_dev_t *p,hc_t *h,int pool) {
    u32 bar=pci_read32(p->bus,p->slot,p->func,0x10),base=bar&~15u;
    if((bar&1) || ((bar&6)!=0 && (bar&6)!=4) || base<0x10000 || base>0xFFFFF000u || (pool<0 && ehci_used>=HC_MAX))
        return ehci_fail("USB: invalid EHCI BAR/pool full");
    if((bar&6)==4 && pci_read32(p->bus,p->slot,p->func,0x14)) return ehci_fail("USB: EHCI BAR above 4 GiB");
    if(paging_map_mmio(base,4096)<0) return ehci_fail("USB: EHCI MMIO map failed");
    u32 cmd=pci_read32(p->bus,p->slot,p->func,4)&0xFFFF;
    pci_write32(p->bus,p->slot,p->func,4,cmd|2); /* decode MMIO, do NOT enable DMA yet */
    volatile u32 *cap=EHCI_MMIO(base);
    unsigned len=cap[0]&255,ports=cap[1]&15;
    if((cap[0]>>16)!=0x100 || len<0x10 || len>0x80 || (len&3) || !ports)
        return ehci_fail("USB: invalid EHCI capabilities");
    if(!ehci_ownership(p,cap[2])) return ehci_fail("USB: EHCI firmware handoff failed");
    memset(h,0,sizeof(*h));h->kind=1;h->ports=(u8)ports;h->op=EHCI_MMIO(base+len);
    h->bus=p->bus;h->slot=p->slot;h->func=p->func;h->pool=(u8)(pool<0?ehci_used++:(unsigned)pool);
    pci_write32(p->bus,p->slot,p->func,4,(cmd|2)&~4u);
    (void)pci_read32(p->bus,p->slot,p->func,4);
    ehci_write(h,8,0);ehci_write(h,0,0);
    if(!ehci_wait(h,4,0x1000,0x1000)) return ehci_init_fail(h,"USB: EHCI stop timed out");
    ehci_write(h,0,2);
    if(!ehci_wait(h,0,2,0)) return ehci_init_fail(h,"USB: EHCI reset timed out");
    ehci_dma_t *s=&ehci_pool[h->pool];u32 periodic,qh,td,data;
    if(!EHCI_DMA(s->periodic,sizeof(s->periodic),4096,&periodic) || !EHCI_DMA(&s->qh,sizeof(s->qh),32,&qh) ||
       !EHCI_DMA(&s->td,sizeof(s->td),32,&td) || !EHCI_DMA(s->data,sizeof(s->data),4096,&data))
        return ehci_init_fail(h,"USB: EHCI DMA unavailable");
    memset(s,0,sizeof(*s));for(unsigned i=0;i<1024;i++) s->periodic[i]=TERM;
    ehci_write(h,8,0);if(cap[2]&1) ehci_write(h,0x10,0);
    ehci_write(h,0x14,periodic);ehci_write(h,0x18,0);ehci_write(h,4,0x3F);
    __sync_synchronize();
    pci_write32(p->bus,p->slot,p->func,4,cmd|6);
    ehci_write(h,0,0x80011); /* empty periodic list keeps FRINDEX advancing */
    if(!ehci_wait(h,4,0x1000,0) || !ehci_frames_wait(h,20)) return ehci_init_fail(h,"USB: EHCI start timed out");
    ehci_write(h,0x40,1);
    for(unsigned i=0;i<ports;i++) ehci_port(h,i,1u<<12,0);
    if(!ehci_frames_wait(h,100)) return ehci_init_fail(h,"USB: EHCI frames stopped");
    return 1;
}
static int ehci_init(pci_dev_t *p,hc_t *h) { return ehci_init_pool(p,h,-1); }
