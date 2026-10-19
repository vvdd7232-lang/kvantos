/* Actual EHCI backend + simulated registers/DMA. SS/CS model is not wire/physical qualification. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#define HC_MAX 4
#define TERM 1u
#define IN 0x69u
#define OUT 0xE1u
#define SETUP 0x2Du
typedef struct { volatile u32 *op;u16 io;u8 kind,ports,dead,bus,slot,func,pool;u32 busy; } hc_t;
typedef struct { hc_t *hc;u32 generation;u8 address,port,speed,tt_hub,tt_port; } usb_disk_t;
static int usb_tt_route(usb_disk_t *d,u32 *caps) {
    if(!d->tt_hub || d->tt_hub>127 || !d->tt_port || d->tt_port>15) return 0;
    *caps=(1u<<30)|((u32)d->tt_hub<<16)|((u32)d->tt_port<<23);return 1;
}
static u32 rd(hc_t *,unsigned);
static void wr(hc_t *,unsigned,u32);
static volatile u32 *mmio(u32);
static u64 clock_now(void);
static u32 physical(const void *);
static void progress(void);
#define EHCI_RD(h,o) rd(h,o)
#define EHCI_WR(h,o,v) wr(h,o,v)
#define EHCI_MMIO(base) mmio(base)
#define EHCI_PAUSE() progress()
#include "../../kernel/usb_ehci.h"

static u32 regs[HC_MAX][1024],cmds[HC_MAX],bars[HC_MAX],legs[HC_MAX],legctl[HC_MAX],hcc[HC_MAX];
static hc_t hosts[HC_MAX];static pci_dev_t devs[HC_MAX];
static u64 ticks;static unsigned reads,short_remaining;static int frozen_clock,pa_mode;
static u8 span[8192] __attribute__((aligned(4096)));
static int issues,mode,stuck_owner,stuck_reset,stuck_stop,stuck_start,stuck_ass,no_ass,stop_frames,map_fail,bad_pa,race;
enum { GOOD,SHORT_IN,SHORT_OUT,STALL,BABBLE,DBE,XACT,MISSED,TIMEOUT,DETACH,CHANGE,HSE,BAD_CURRENT,BAD_NEXT,BAD_OVERLAY,OVERREPORT,DISAGREE,POST_DETACH,DELAYED,TT_BUSY,TT_NYET,TT_BAD_TD,TT_BAD_QH,TT_BAD_BOTH };
static ehci_dma_t *pending,*split;
static unsigned phase,steps,ss_count,nyet_count,cs_count;

static u32 raw(hc_t *h,unsigned r) { return h->op[r/4]; }
static void put(hc_t *h,unsigned r,u32 v) { h->op[r/4]=v; }
u32 pci_read32(u8 b,u8 s,u8 f,u8 off) {
    (void)b;(void)f;assert(s<HC_MAX);
    if(off==4) return cmds[s];if(off==0x10) return bars[s];if(off==0x14) return hcc[s];
    if(off==0x80) return legs[s];if(off==0x84) return legctl[s];return 0;
}
void pci_write32(u8 b,u8 s,u8 f,u8 off,u32 v) {
    (void)b;(void)f;assert(s<HC_MAX);
    if(off==4) { assert(!(v&0xFFFF0000));cmds[s]=v;return; }
    if(off==0x80) { legs[s]=v;if(!stuck_owner) legs[s]&=~(1u<<16);return; }
    assert(off==0x84 && !(v&0xFFFF));legctl[s]&=~v;
}
int paging_map_mmio(u32 base,u32 bytes) { assert(bytes==4096);return map_fail || base<0x10000?-1:0; }
static volatile u32 *mmio(u32 base) {
    for(int i=0;i<HC_MAX;i++) {
        if(base==(bars[i]&~15u)) return regs[i];
        if(base==(bars[i]&~15u)+0x20) return regs[i]+8;
    }
    assert(0);return NULL;
}
static u64 clock_now(void) { if(!frozen_clock) ticks+=1000000;return ticks; }
static u32 physical(const void *p) {
    if(bad_pa) return 0;
    if(pa_mode==1) return 0xFFFFFFE0;
    if(pa_mode==2) return (u32)(kv_addr_t)p+1;
    return (u32)(kv_addr_t)p+(pa_mode==3 && p==(const void *)(span+4096)?4096:0);
}
static u32 rd(hc_t *h,unsigned r) { reads++;return raw(h,r); }
static void complete(ehci_dma_t *s,unsigned len) {
    unsigned remaining=(mode==SHORT_IN || mode==SHORT_OUT)?short_remaining:mode==OVERREPORT?len+1:0;
    u32 errors=mode==STALL?0x40:mode==BABBLE?0x50:mode==DBE?0x60:mode==XACT?0x48:mode==MISSED?0x44:0;
    s->td.token=(s->td.token&~0x7FFF00FCu)|(remaining<<16)|errors;
    s->qh.current=physical(&s->td);s->qh.overlay.next=TERM;s->qh.overlay.token=s->td.token;
    if(mode==BAD_CURRENT) s->qh.current+=32;
    if(mode==BAD_NEXT) s->qh.overlay.next=physical(&s->td);
    if(mode==BAD_OVERLAY) s->qh.overlay.token|=0x80;
    if(mode==DISAGREE) s->qh.overlay.token|=1u<<16;
}
static void issue(hc_t *h) {
    issues++;ehci_dma_t *s=&ehci_pool[h->pool];
    assert(cmds[h->slot]&4);assert(!raw(h,8));
    assert(raw(h,0x18)==physical(&s->qh) && raw(h,0x14)==physical(s->periodic));
    assert(s->qh.link==(physical(&s->qh)|2));
    unsigned speed=(s->qh.endpoint>>12)&3,ep=(s->qh.endpoint>>8)&15;
    assert(speed==0 || speed==2);
    assert(s->qh.caps==(speed==2?1u<<30:(1u<<30)|(11u<<16)|(7u<<23)));
    assert((s->qh.endpoint&(1u<<27))==(speed==0 && !ep?1u<<27:0));
    assert((s->qh.endpoint>>28)==(speed==2?8u:0u));
    assert( (s->qh.endpoint&(1u<<14)) && (s->qh.endpoint&(1u<<15)));
    assert(s->qh.overlay.next==physical(&s->td) && s->td.next==TERM && s->td.alt==TERM);
    unsigned len=(s->td.token>>16)&0x7FFF,pid=(s->td.token>>8)&3;
    assert(len<=4096 && s->td.page[0]==(len?physical(s->data):0));
    for(unsigned i=1;i<5;i++) assert(!s->td.page[i]);
    for(unsigned i=0;i<5;i++) assert(!s->td.upper[i]);
    if(pid==1) memset(s->data,0x3C,len);
    if(!speed) { split=s;phase=steps=0;s->qh.current=physical(&s->td);s->qh.overlay.token=s->td.token;return; }
    if(mode==TIMEOUT) return;
    if(mode==DETACH) { put(h,0x44,0);return; }
    if(mode==CHANGE) { put(h,0x44,raw(h,0x44)|2);return; }
    if(mode==HSE) { put(h,4,raw(h,4)|16);return; }
    complete(s,len);
    if(mode==DELAYED) { s->qh.overlay.token|=0x80;s->qh.overlay.next=physical(&s->td);pending=s; }
}
static void wr(hc_t *h,unsigned r,u32 v) {
    if(r==4) { put(h,r,raw(h,r)&~(v&0x3F));return; }
    if(r==0) {
        u32 old=raw(h,0);put(h,r,v);
        if(v&2) { if(!stuck_reset) put(h,0,0);put(h,4,0x1000);return; }
        if(!(v&1)) { if(!stuck_stop) put(h,4,0x1000);return; }
        if(stuck_start) { put(h,4,0x1000);return; }
        put(h,4,raw(h,4)&~0x1000u);
        if(v&32) {
            if(!no_ass) put(h,4,raw(h,4)|0x8000);
            issue(h);
        } else {
            if(!stuck_ass) put(h,4,raw(h,4)&~0x8000u);
            if(old&32) {
                if(mode==POST_DETACH) put(h,0x44,raw(h,0x44)|2);
                if(pending && !stuck_ass) { pending->qh.overlay.token=pending->td.token;pending->qh.overlay.next=TERM;pending=NULL; }
            }
        }
        return;
    }
    if(r>=0x44 && r<0x44u+4u*h->ports) {
        u32 old=raw(h,r);u32 v2=(v&~0x2Au)|(old&0x2A&~v);
        if((old&0x100) && !(v&0x100) && !(v&0x2000)) v2|=4;
        put(h,r,v2);return;
    }
    put(h,r,v);
    if(r==0x18 && v && race) put(h,0x44,raw(h,0x44)|2);
}
static void progress(void) {
    for(unsigned i=0;i<HC_MAX;i++)
        if(!stop_frames && (regs[i][0x20/4]&1) && !(regs[i][(0x20+4)/4]&0x1000)) regs[i][(0x20+0xC)/4]+=8;
    if(split) {
        /* SS NAK / SS ACK -> CS NYET -> CS result. qTD remains active until CS. */
        assert(split->td.token&0x80);steps++;
        if(!phase) {
            ss_count++;if(mode==TT_BUSY || steps<3) return;
            split->qh.overlay.token|=2;phase=1;return;
        }
        if(phase==1) {
            assert(split->qh.overlay.token&2);nyet_count++;
            if(mode==TT_NYET || mode==TIMEOUT || steps<6) return;
            phase=2;return;
        }
        cs_count++;ehci_dma_t *s=split;split=NULL;
        unsigned len=(s->td.token>>16)&0x7FFF;
        if(mode==DETACH || mode==CHANGE) { put(&hosts[0],0x44,mode==DETACH?0:7);return; }
        if(mode==HSE) { put(&hosts[0],4,raw(&hosts[0],4)|16);return; }
        split=NULL;s->qh.overlay.token&=~2u;complete(s,len);
        if(mode==TT_BAD_TD || mode==TT_BAD_BOTH) s->td.token|=2;
        if(mode==TT_BAD_QH || mode==TT_BAD_BOTH) s->qh.overlay.token|=2;
    }
}
static void reset(void) {
    memset(regs,0,sizeof(regs));memset(cmds,0,sizeof(cmds));memset(hosts,0,sizeof(hosts));memset(legs,0,sizeof(legs));memset(hcc,0,sizeof(hcc));
    memset(ehci_pool,0,sizeof(ehci_pool));memset((void *)ehci_faults,0,sizeof(ehci_faults));ehci_used=0;issues=mode=stuck_owner=stuck_reset=stuck_stop=stuck_start=stuck_ass=no_ass=stop_frames=map_fail=bad_pa=race=0;ticks=reads=frozen_clock=pa_mode=0;short_remaining=5;pending=split=NULL;phase=steps=ss_count=nyet_count=cs_count=0;
    for(unsigned i=0;i<HC_MAX;i++) {
        bars[i]=0xF0000000+i*0x10000;devs[i].slot=i;devs[i].bus=devs[i].func=0;
        regs[i][0]=0x01000020;regs[i][1]=2;regs[i][2]=1;
        regs[i][(0x20+4)/4]=0x1000;regs[i][(0x20+0x44)/4]=5;
    }
}
static void init(unsigned slot,unsigned at) { assert(ehci_init(&devs[slot],&hosts[at]));assert(hosts[at].pool==ehci_used-1); }
static usb_disk_t disk(unsigned at) { usb_disk_t d={.hc=&hosts[at],.generation=1,.address=5,.speed=2};return d; }
static void transfer_tests(void) {
    for(unsigned bytes=0;bytes<=4096;bytes+=bytes<512?1:512) {
        reset();init(0,0);usb_disk_t d=disk(0);u8 b[4096];memset(b,0xA5,sizeof(b));int t=1;
        assert(ehci_transfer(&d,IN,1,512,&t,b,bytes)==(int)bytes);
        assert(t==(int)(1^((bytes?(bytes+511)/512:1)&1)));
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==(j<bytes?0x3C:0xA5));
    }
    reset();init(0,0);usb_disk_t d=disk(0);u8 b[4096];memset(b,0x91,sizeof(b));int t=0;
    assert(ehci_transfer(&d,OUT,1,512,&t,b,sizeof(b))==4096 && !t);
    assert(!memcmp(ehci_pool[0].data,b,sizeof(b)));
    t=0;assert(ehci_transfer(&d,SETUP,0,64,&t,b,8)==8 && t==1);
    mode=SHORT_IN;t=0;assert(ehci_transfer(&d,IN,1,512,&t,b,1024)==1019 && !t);
    for(unsigned actual=0;actual<1024;actual+=actual<512?1:511) {
        short_remaining=1024-actual;t=0;memset(b,0xA5,sizeof(b));
        assert(ehci_transfer(&d,IN,1,512,&t,b,1024)==(int)actual);
        assert(t==(int)((actual/512+1)&1));
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==(j<actual?0x3C:0xA5));
    }
    mode=DELAYED;t=0;assert(ehci_transfer(&d,IN,1,512,&t,b,512)==512 && t==1 && !pending);
    unsigned n=issues;hosts[0].busy=1;assert(ehci_transfer(&d,IN,1,512,&t,b,512)<0 && issues==(int)n && hosts[0].busy);hosts[0].busy=0;
    assert(ehci_transfer(&d,IN,1,64,&t,b,1)<0 && !hosts[0].dead);
    assert(ehci_transfer(&d,IN,16,512,&t,b,1)<0);
    assert(ehci_transfer(&d,IN,1,512,&t,b,4097)<0);
    assert(ehci_transfer(&d,SETUP,0,64,&t,b,8)<0); /* SETUP toggle must be 0 */
    assert(ehci_transfer(&d,OUT,1,512,&t,NULL,1)<0);
    assert(ehci_transfer(&d,IN,1,512,NULL,b,1)<0);
    t=2;assert(ehci_transfer(&d,IN,1,512,&t,b,1)<0);t=0;
    assert(ehci_transfer(&d,0xFF,1,512,&t,b,1)<0);
    assert(ehci_transfer(&d,SETUP,0,64,&t,b,7)<0);
    d.port=hosts[0].ports;assert(ehci_transfer(&d,IN,1,512,&t,b,1)<0);d.port=0;
    d.address=128;assert(ehci_transfer(&d,IN,1,512,&t,b,1)<0);d.address=5;
    hosts[0].pool=HC_MAX;assert(ehci_transfer(&d,IN,1,512,&t,b,1)<0);
    assert(!hosts[0].dead);
}
static void split_tests(void) {
    static const unsigned sizes[]={0,1,7,8,9,15,16,17,31,32,33,63,64,65,511,512,513,1024,4096};
    for(unsigned mps=8;mps<=64;mps*=2) for(unsigned ep=0;ep<=1;ep++) for(unsigned k=0;k<sizeof(sizes)/sizeof(*sizes);k++) {
        reset();init(0,0);usb_disk_t d=disk(0);d.speed=0;d.tt_hub=11;d.tt_port=7;
        u8 b[4096];memset(b,0xA5,sizeof(b));int t=1;unsigned len=sizes[k];
        assert(ehci_transfer(&d,IN,ep,mps,&t,b,len)==(int)len);
        assert(ss_count>=3 && nyet_count>=3 && cs_count==1 && !split);
        assert(t==(int)(1^((len?(len+mps-1)/mps:1)&1)));
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==(j<len?0x3C:0xA5));
        memset(b,0xA5,sizeof(b));t=0;
        assert(ehci_transfer(&d,OUT,ep,mps,&t,b,len)==(int)len);
        assert(t==(int)((len?(len+mps-1)/mps:1)&1));
    }
    reset();init(0,0);usb_disk_t d=disk(0);d.speed=0;d.tt_hub=11;d.tt_port=7;
    u8 b[1024];memset(b,0xA5,sizeof(b));int t=0;
    assert(ehci_transfer(&d,SETUP,0,8,&t,b,8)==8 && t==1);
    mode=SHORT_IN;short_remaining=5;t=0;
    assert(ehci_transfer(&d,IN,1,64,&t,b,512)==507 && !t);
    unsigned n=issues;d.tt_port=0;assert(ehci_transfer(&d,IN,1,64,&t,b,512)<0 && issues==(int)n && !hosts[0].dead);
    d.tt_port=7;d.tt_hub=128;assert(ehci_transfer(&d,IN,1,64,&t,b,512)<0 && !hosts[0].dead);
    d.tt_hub=11;assert(ehci_transfer(&d,IN,1,512,&t,b,512)<0 && !hosts[0].dead);
    d.speed=1;assert(ehci_transfer(&d,IN,0,8,&t,b,8)<0 && !hosts[0].dead);
    puts("PASS FS TT: EP0 8/16/32/64, bulk 8/16/32/64, ZLP/short/data/toggles; SS NAK/ACK -> CS NYET -> result, route/MPS/LS refusals before DMA");
    for(int fault=SHORT_OUT;fault<=TT_BAD_BOTH;fault++) {
        if(fault==DELAYED) continue;
        reset();init(0,0);usb_disk_t d=disk(0);d.speed=0;d.tt_hub=11;d.tt_port=7;
        memset(b,0xA5,sizeof(b));t=1;mode=fault;
        assert(ehci_transfer(&d,fault==SHORT_OUT?OUT:IN,1,64,&t,b,sizeof(b))<0);
        assert(hosts[0].dead && !(cmds[0]&4) && t==1);
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==0xA5);
        unsigned n=issues;assert(ehci_transfer(&d,IN,1,64,&t,b,512)<0 && issues==(int)n);
        /* A fresh HS transfer on this HC MUST also remain fenced. */
        d.speed=2;assert(ehci_transfer(&d,IN,1,512,&t,b,512)<0 && issues==(int)n);
    }
    for(unsigned which=0;which<2;which++) {
        reset();init(0,0);d=disk(0);d.speed=0;d.tt_hub=11;d.tt_port=7;
        memset(b,0xA5,sizeof(b));t=1;stuck_ass=which==0;no_ass=which==1;
        assert(ehci_transfer(&d,IN,1,64,&t,b,512)<0 && hosts[0].dead && !(cmds[0]&4) && t==1);
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==0xA5);
    }
    reset();init(0,0);d=disk(0);d.speed=0;d.tt_hub=11;d.tt_port=7;
    frozen_clock=stop_frames=1;mode=TT_NYET;memset(b,0xA5,sizeof(b));t=1;
    unsigned start_reads=reads;
    assert(ehci_transfer(&d,IN,1,64,&t,b,512)<0 && hosts[0].dead && !(cmds[0]&4) && t==1);
    assert(reads-start_reads<EHCI_POLL_ITERS*8u+2000002u);
    for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==0xA5);
    reset();init(0,0);init(1,1);d=disk(0);d.speed=0;d.tt_hub=11;d.tt_port=7;
    mode=TT_NYET;t=1;assert(ehci_transfer(&d,IN,1,64,&t,b,512)<0);
    memset(&ehci_pool[0],0xEE,sizeof(ehci_pool[0])); /* simulated late DMA */
    d=disk(1);mode=GOOD;t=0;assert(ehci_transfer(&d,IN,1,512,&t,b,512)==512 && b[0]==0x3C);
    puts("PASS FS TT faults: busy/NYET forever, errors/disconnect/malformed/ambiguous SplitXState -> whole-HC BME off, unchanged caller/toggle, no HS/FS replay; frozen clocks bounded, other HC survives late DMA");
}
static void diagnostic_tests(void) {
    const int faults[]={XACT,TT_BAD_BOTH,BAD_CURRENT,SHORT_OUT,DETACH,TT_NYET,TT_NYET,TT_NYET};
    const unsigned reasons[]={EHCI_TOKEN,EHCI_SPLIT,EHCI_WRITEBACK,EHCI_SHORT,EHCI_LINK,EHCI_TSC,EHCI_FRAMES,EHCI_BUDGET};
    for(unsigned i=0;i<sizeof(faults)/sizeof(*faults);i++) {
        reset();init(0,0);usb_disk_t d=disk(0);d.speed=0;d.tt_hub=11;d.tt_port=7;
        u8 b[512];memset(b,0xA5,sizeof(b));int t=1;mode=faults[i];
        frozen_clock=i>=6;stop_frames=i==5 || i==7;
        /* Distinguish a running frame counter from frozen frame/TSC budgets. */
        if(i==6) put(&hosts[0],0xC,2040u<<3); /* FS NYET + FRINDEX wrap */
        assert(ehci_transfer(&d,i==3?OUT:IN,1,d.speed?512:64,&t,b,sizeof(b))<0);
        const volatile ehci_fault_t *f=&ehci_faults[0];
        assert(f->reason==reasons[i] && f->retired==1 && f->address==5 && f->generation==1 && f->len==512);
        assert(f->stop_tsc>=f->start_tsc && f->iterations<=EHCI_POLL_ITERS && !hosts[0].busy && hosts[0].dead && !(cmds[0]&4));
        if(i>=5) assert(f->pre_td&0x80);
        if(i>=5) assert(f->pre_overlay&2);
        if(i==6) assert(((f->stop_frame-f->start_frame)&2047)>=EHCI_POLL_FRAMES);
        if(i==7) assert(f->iterations==EHCI_POLL_ITERS);
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==0xA5);
        assert(t==1);
    }
    reset();init(0,0);usb_disk_t d=disk(0);u8 b[512];int t=0;
    assert(ehci_transfer(&d,IN,1,512,&t,b,sizeof(b))==512 && !ehci_faults[0].reason);
    for(unsigned which=0;which<2;which++) {
        reset();init(0,0);d=disk(0);t=0;no_ass=which==0;stuck_ass=which==1;
        assert(ehci_transfer(&d,IN,1,512,&t,b,sizeof(b))<0);
        assert(ehci_faults[0].reason==(which?EHCI_ASS:EHCI_START));
        assert(!ehci_faults[0].retired && hosts[0].dead);
    }
    puts("PASS EHCI retained diagnostics: raw TSC/frame/iteration budget vs link/token/split/writeback/short/ASS failures; pre/post writeback, unchanged caller and original bounds");
}
int main(void) {
    diagnostic_tests();split_tests();transfer_tests();puts("PASS EHCI actual backend: control/BOT/ZLP/short IN/0..4096 bytes/toggles/busy/bounds, retired QH before copyout");
    for(int fault=SHORT_OUT;fault<=POST_DETACH;fault++) {
        reset();init(0,0);usb_disk_t d=disk(0);u8 b[1024];memset(b,0xA5,sizeof(b));int t=1;mode=fault;
        assert(ehci_transfer(&d,fault==SHORT_OUT?OUT:IN,1,512,&t,b,sizeof(b))<0);
        assert(hosts[0].dead && !(cmds[0]&4) && t==1);
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==0xA5);
        int count=issues;assert(ehci_transfer(&d,IN,1,512,&t,b,512)<0 && issues==count);
    }
    for(int which=0;which<3;which++) {
        reset();init(0,0);usb_disk_t d=disk(0);u8 b[512];memset(b,0xA5,sizeof(b));int t=0;
        stuck_ass=which==0;no_ass=which==1;race=which==2;
        assert(ehci_transfer(&d,IN,1,512,&t,b,512)<0 && hosts[0].dead && !(cmds[0]&4) && !t);
        if(race) assert(!issues);
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==0xA5);
    }
    puts("PASS EHCI stall/babble/buffer/xact/missed/timeout/detach/change/HSE/malformed writeback/ASS failures: fence, unchanged caller/toggle, no replay");
    reset();init(0,0);init(1,1);assert(hosts[0].pool!=hosts[1].pool);
    usb_disk_t a=disk(0),b=disk(1);u8 buf[512];int t=0;mode=TIMEOUT;
    assert(ehci_transfer(&a,IN,1,512,&t,buf,512)<0);mode=GOOD;
    memset(ehci_pool[0].data,0xEE,4096);assert(ehci_transfer(&b,IN,1,512,&t,buf,512)==512 && buf[0]==0x3C);
    reset();stop_frames=1;assert(!ehci_init(&devs[0],&hosts[0]) && hosts[0].dead && ehci_used==1);
    stop_frames=0;init(1,0);assert(hosts[0].pool==1);u8 saved[sizeof(ehci_dma_t)];memcpy(saved,&ehci_pool[1],sizeof(saved));
    memset(&ehci_pool[0],0xEE,sizeof(ehci_pool[0]));assert(!memcmp(saved,&ehci_pool[1],sizeof(saved)));
    init(2,1);init(3,2);assert(!ehci_init(&devs[0],&hosts[3]) && ehci_used==HC_MAX);
    puts("PASS EHCI private pools: two HC, runtime fence, failed-start slot reuse, simulated late DMA, bounded attempt pool");
    for(int fail=0;fail<9;fail++) {
        reset();if(fail==0) bars[0]|=1;if(fail==1) { bars[0]|=4;hcc[0]=1; }
        if(fail==2) map_fail=1;if(fail==3) regs[0][0]=0x01010020;
        if(fail==4) { regs[0][2]|=0x8000;legs[0]=0x10001;stuck_owner=1; }
        if(fail==5) stuck_stop=1;if(fail==6) stuck_reset=1;if(fail==7) bad_pa=1;if(fail==8) stuck_start=1;
        if(stuck_stop) regs[0][(0x20+4)/4]=0;
        assert(!ehci_init(&devs[0],&hosts[0]) && !(cmds[0]&4));
    }
    reset();regs[0][2]|=0x8000;legs[0]=0x10001;legctl[0]=0x1234FFFF;init(0,0);
    assert(legs[0]&(1u<<24));assert(!(legs[0]&(1u<<16)) && legctl[0]==0xFFFF);
    assert(ehci_reset_port(&hosts[0],0));assert(!ehci_reset_port(&hosts[0],15));
    reset();u32 pa;
    assert(ehci_dma_address(span,sizeof(span),4096,&pa));
    assert(!ehci_dma_address(span,0,4096,&pa));
    assert(!ehci_dma_address((void *)(kv_addr_t)0x100000000ULL,32,32,&pa));
    for(pa_mode=1;pa_mode<=3;pa_mode++) assert(!ehci_dma_address(span,sizeof(span),32,&pa));
    reset();regs[0][2]|=0x8000;legs[0]=0x8002;assert(!ehci_init(&devs[0],&hosts[0]) && !ehci_used);
    reset();bars[0]|=4;init(0,0); /* 64-bit BAR with a zero upper half is DMA32-compatible */
    frozen_clock=1;stop_frames=1;unsigned start_reads=reads;
    assert(!ehci_frames_wait(&hosts[0],20));assert(reads-start_reads<=2000002);
    start_reads=reads;assert(!ehci_wait(&hosts[0],4,0x8000,0x8000));assert(reads-start_reads==1000000);
    u8 caller[512];memset(caller,0xA5,sizeof(caller));usb_disk_t d=disk(0);int toggle=1;
    mode=TIMEOUT;start_reads=reads;
    assert(ehci_transfer(&d,IN,1,512,&toggle,caller,sizeof(caller))<0 && hosts[0].dead && toggle==1);
    assert(reads-start_reads<EHCI_POLL_ITERS*8u+2000002u);
    for(unsigned i=0;i<sizeof(caller);i++) assert(caller[i]==0xA5);
    reset();init(0,0);d=disk(0);bad_pa=1;
    assert(ehci_transfer(&d,IN,1,512,&toggle,caller,sizeof(caller))<0 && hosts[0].dead && !issues);
    puts("PASS EHCI BAR/capability/firmware/stop/reset/map/DMA/start refusals + BIOS handoff/root reset");
    puts("PASS DMA32 overflow/alignment/page gaps; cyclic capabilities; frozen TSC + stopped frames/ASS bounded by iteration budgets");
    return 0;
}
