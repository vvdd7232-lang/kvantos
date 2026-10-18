/* Actual OHCI backend, simulated MMIO/DMA only. Not physical qualification. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#define HC_MAX 4
#define IN 0x69u
#define OUT 0xE1u
#define SETUP 0x2Du
typedef struct { volatile u32 *op; u16 io; u8 kind,ports,dead,bus,slot,func,pool; u32 busy; } hc_t;
typedef struct { hc_t *hc; u32 generation; u8 address,port; } usb_disk_t;
static u32 rd(hc_t *,unsigned);
static void wr(hc_t *,unsigned,u32);
static volatile u32 *mmio(u32);
static u64 clock_now(void);
static u32 physical(const void *);
static void progress(void);
static int frames_wait(hc_t *,unsigned);
#define OHCI_RD(h,o) rd(h,o)
#define OHCI_WR(h,o,v) wr(h,o,v)
#define OHCI_MMIO(base) mmio(base)
#define OHCI_PAUSE() progress()
#include "../../kernel/usb_ohci.h"

static u32 regs[HC_MAX][256],cmds[HC_MAX],bars[HC_MAX];
static hc_t hosts[HC_MAX];
static pci_dev_t devices[HC_MAX];
static u64 ticks;
static int stuck_owner,stuck_reset,stop_frames,phys_gap,map_fail,mode,issues,race,retire_error;
enum { GOOD, SHORT, STALL, TIMEOUT, DETACH, UE, BAD_CBP, BAD_HEAD, SHORT_OUT, PARTIAL };
u32 pci_read32(u8 b,u8 s,u8 f,u8 off) { (void)b;(void)f;return off==4?cmds[s]:bars[s]; }
void pci_write32(u8 b,u8 s,u8 f,u8 off,u32 v) { (void)b;(void)f;assert(off==4 && !(v&0xFFFF0000));cmds[s]=v; }
int paging_map_mmio(u32 base,u32 bytes) { assert(bytes==4096);return map_fail || base<0xF0000000?-1:0; }
static volatile u32 *mmio(u32 base) {
    for(int i=0;i<HC_MAX;i++) if(base==(bars[i]&~15u)) return regs[i];
    assert(0);return NULL;
}
static u64 clock_now(void) { ticks+=1000000;return ticks; }
static u32 physical(const void *p) { return (u32)(kv_addr_t)p+(phys_gap && (kv_addr_t)p>=(kv_addr_t)ohci_pool+4096?4096:0); }
static u32 rd(hc_t *h,unsigned r) { return h->op[r/4]; }
static hc_t *pending_host;
static ohci_td_t pending_td;
static unsigned pending_ticks;
static int pending_bulk;
static void issue(hc_t *h,int bulk) {
    issues++;
    assert(cmds[h->slot]&4);
    assert((rd(h,4)&0xC0)==0x80 && (rd(h,4)&(bulk?32:16)));
    ohci_ed_t *ed=(void *)(kv_addr_t)rd(h,bulk?0x28:0x20);
    assert(ed==&ohci_pool[h->pool].ed && rd(h,0x18)==(u32)(kv_addr_t)&ohci_pool[h->pool].hcca);
    assert(!((kv_addr_t)ed&15) && !(ed->flags&(1u<<14)) && !(ed->flags&(3u<<11)));
    ohci_td_t *td=(void *)(kv_addr_t)(ed->head&~15u);
    assert(ed->tail==(u32)(kv_addr_t)(td+1) && td->next==ed->tail && td->flags>>28==15);
    assert(((td->flags>>24)&3)>=2 && !(td->flags&(7u<<21)));
    unsigned dp=(td->flags>>19)&3,mps=(ed->flags>>16)&2047;
    assert(td==ohci_pool[h->pool].td && (!td->cbp || td->cbp==(u32)(kv_addr_t)ohci_pool[h->pool].data));
    unsigned len=td->cbp?td->end-td->cbp+1:0;
    assert(mps && mps<=64 && len<=4096);
    if(!len) assert(!td->end);
    if(mode==PARTIAL) {
        assert(dp==1 && len>mps);
        pending_host=h;pending_td=*td;pending_ticks=3;pending_bulk=bulk;
        td->flags&=0x0FFFFFFFu;td->cbp+=mps; /* CC=0, ED STILL points to TD */
        return;
    }
    if(mode==TIMEOUT) return;
    if(mode==DETACH) { h->op[0x54/4]=0;return; }
    if(mode==UE) { h->op[0x0C/4]|=16;return; }
    u32 data=td->cbp;
    unsigned actual=(mode==SHORT || mode==SHORT_OUT)?len/2:len;
    if(mode==SHORT) assert(dp==2 && (td->flags&(1u<<18)));
    for(unsigned i=0;i<actual;i++) {
        if(dp==2) ((u8 *)(kv_addr_t)data)[i]=(u8)(i*13+7);
        else assert(((u8 *)(kv_addr_t)data)[i]==(u8)(i*13+7));
    }
    unsigned packets=actual<len?actual/mps+1:len?(len+mps-1)/mps:1;
    unsigned toggle=(((td->flags>>24)&1)^(packets&1))<<1;
    ed->head=ed->tail|toggle;
    td->flags&=0x0FFFFFFF;
    td->cbp=actual<len?data+actual:0;
    td->next=0; /* actual done-list writeback may overwrite NextTD */
    if(mode==STALL) { td->flags|=4u<<28;ed->head|=1; }
    if(mode==BAD_CBP) td->cbp=data-1;
    if(mode==BAD_HEAD) ed->head=(u32)(kv_addr_t)td;
}
static void wr(hc_t *h,unsigned r,u32 v) {
    if(r==0x0C) { h->op[r/4]&=~v;if(v==2 && race) { h->op[0x54/4]|=1u<<16;race=0; }return; }
    if(r==8) {
        if(v&8) { if(!stuck_owner) h->op[1]&=~256u;return; }
        if(v&1) { h->op[r/4]=stuck_reset?1:0;return; }
        h->op[r/4]=0;if(v&6) issue(h,!!(v&4));return;
    }
    if(r>=0x54 && r<0x90) {
        if(v&(1u<<8)) h->op[r/4]|=1u<<8;
        if(v&(1u<<4)) h->op[r/4]|=3u|(1u<<20);
        h->op[r/4]&=~(v&0x1F0000u);return;
    }
    if(r==0x18) assert(!(cmds[h->slot]&4) && !(rd(h,4)&256));
    h->op[r/4]=v;
}
static void progress(void) {
    ticks+=1000000;for(int i=0;i<HC_MAX;i++) if(!stop_frames) regs[i][0x3C/4]++;
    if(pending_ticks && !--pending_ticks) {
        ohci_ed_t *ed=(void *)(kv_addr_t)rd(pending_host,pending_bulk?0x28:0x20);
        ohci_td_t *td=(void *)(kv_addr_t)(ed->head&~15u);
        *td=pending_td;mode=GOOD;issues--;issue(pending_host,pending_bulk);
    }
}
static int frames_wait(hc_t *h,unsigned n) {
    if(stop_frames || h->dead || (rd(h,0x0C)&16)) return 0;
    h->op[0x3C/4]+=n;
    if(retire_error) h->op[0x0C/4]|=16;
    return 1;
}
static void reset(void) {
    memset(regs,0,sizeof(regs));memset(hosts,0,sizeof(hosts));memset(devices,0,sizeof(devices));
    memset(ohci_pool,0,sizeof(ohci_pool));ohci_used=0;ohci_count=ohci_seen=0;ticks=0;pending_ticks=0;
    stuck_owner=stuck_reset=stop_frames=phys_gap=map_fail=mode=issues=race=retire_error=0;
    for(int i=0;i<HC_MAX;i++) {
        bars[i]=0xF0000000+i*4096;cmds[i]=6;devices[i].slot=(u8)i;
        regs[i][0]=0x10;regs[i][1]=256;regs[i][0x34/4]=11999;
        regs[i][0x48/4]=3;regs[i][0x54/4]=1;
    }
}
static usb_disk_t ready(int n) {
    assert(ohci_init(devices+n,hosts+n));assert(ohci_reset_port(hosts+n,0));
    wr(hosts+n,0x54,0x1F0000);
    return (usb_disk_t){.hc=hosts+n,.generation=1,.address=3};
}
int main(void) {
    reset();u32 address;
    assert(!ohci_dma_address(NULL,4096,4096,&address));
    assert(!ohci_dma_address((void *)((kv_addr_t)1<<32),4096,4096,&address));
    assert(!ohci_dma_address((void *)0xFFFFFFF0u,4096,16,&address));
    assert(!ohci_dma_address(&ohci_pool[0].data[1],100,4096,&address));
    assert(ohci_dma_address(ohci_pool,sizeof(ohci_pool),4096,&address));
    phys_gap=1;assert(!ohci_dma_address(ohci_pool,sizeof(ohci_pool),4096,&address));phys_gap=0;
    usb_disk_t d=ready(0);assert(ohci_used==1 && hosts[0].kind==2 && hosts[0].ports==3);
    static u8 data[4098];int toggle=0;
    memset(data,0xA5,sizeof(data));
    assert(ohci_transfer(&d,IN,2,64,&toggle,data+1,4096)==4096 && toggle==0);
    assert(data[0]==0xA5 && data[4097]==0xA5);
    for(int i=0;i<4096;i++) assert(data[i+1]==(u8)(i*13+7));
    assert(ohci_transfer(&d,OUT,1,64,&toggle,data+1,4096)==4096);
    assert(ohci_transfer(&d,IN,0,8,&toggle,NULL,0)==0 && toggle==1);
    toggle=0;assert(ohci_transfer(&d,SETUP,0,8,&toggle,data+1,8)==8 && toggle==1);
    mode=PARTIAL;toggle=0;
    assert(ohci_transfer(&d,OUT,1,64,&toggle,data+1,128)==128 && !hosts[0].dead && toggle==0);
    puts("PASS OHCI: intermediate OUT packet CC=NoError is NOT completion; wait for ED retirement");
    mode=SHORT;toggle=1;assert(ohci_transfer(&d,IN,1,64,&toggle,data+1,64)==32 && toggle==0);
    int count=issues;hosts[0].busy=1;assert(ohci_transfer(&d,IN,1,64,&toggle,data,64)<0 && issues==count);hosts[0].busy=0;
    assert(ohci_transfer(&d,IN,1,64,&toggle,NULL,1)<0 && !hosts[0].dead);
    assert(ohci_transfer(&d,IN,1,64,&toggle,data,4097)<0 && !hosts[0].dead);
    assert(ohci_transfer(&d,IN,16,64,&toggle,data,64)<0 && !hosts[0].dead);
    assert(ohci_transfer(&d,IN,1,3,&toggle,data,64)<0 && !hosts[0].dead);
    assert(ohci_transfer(&d,IN,1,64,NULL,data,64)<0 && !hosts[0].dead);
    puts("PASS OHCI: firmware handoff, IRQ-off/HCCA/ED/TD, DMA OUT/IN/SETUP/ZLP, short IN, carry, bounds/busy");
    for(int fault=STALL;fault<=SHORT_OUT;fault++) {
        reset();d=ready(0);mode=fault;toggle=1;memset(data,0xA5,sizeof(data));
        if(fault==SHORT_OUT) for(int i=0;i<64;i++) data[i]=(u8)(i*13+7);
        assert(ohci_transfer(&d,fault==SHORT_OUT?OUT:IN,1,64,&toggle,data,64)<0);
        assert(hosts[0].dead && !hosts[0].busy && !(cmds[0]&4) && toggle==1);
        if(fault!=SHORT_OUT) for(int i=0;i<64;i++) assert(data[i]==0xA5);
        count=issues;assert(ohci_transfer(&d,IN,1,64,&toggle,data,64)<0 && issues==count);
    }
    reset();d=ready(0);race=1;toggle=0;assert(ohci_transfer(&d,IN,1,64,&toggle,data,64)<0 && !issues);
    reset();d=ready(0);retire_error=1;toggle=0;memset(data,0xA5,64);
    assert(ohci_transfer(&d,IN,1,64,&toggle,data,64)<0 && hosts[0].dead && data[0]==0xA5);
    reset();d=ready(0);stop_frames=1;toggle=0;memset(data,0xA5,64);
    assert(ohci_transfer(&d,IN,1,64,&toggle,data,64)<0 && hosts[0].dead && data[0]==0xA5);
    puts("PASS OHCI: stall/timeout/disconnect/UE/bad CBP/head/short OUT/retire fault fence; no copy/replay, link latch pre-issue race");
    reset();d=ready(0);mode=TIMEOUT;toggle=0;assert(ohci_transfer(&d,IN,1,64,&toggle,data,64)<0);
    ohci_dma_t *old=&ohci_pool[hosts[0].pool];
    mode=GOOD;usb_disk_t newer=ready(1);assert(hosts[1].pool!=hosts[0].pool);
    memset(old->data,0xFF,sizeof(old->data)); /* late DMA into fenced private pool */
    assert(ohci_transfer(&newer,IN,1,64,&toggle,data,64)==64 && data[0]==7);
    assert(old->data[0]==0xFF);
    reset();stop_frames=1;assert(!ohci_init(devices,hosts) && ohci_used==1);
    stop_frames=0;assert(ohci_init(devices+1,hosts+1) && hosts[1].pool==1);
    puts("PASS OHCI: separate per-attempt DMA pools; failed start and late completion cannot recycle another controller's buffers");
    reset();for(int i=0;i<HC_MAX;i++) (void)ready(i);
    assert(!ohci_init(devices,hosts) && ohci_used==HC_MAX);
    reset();stuck_owner=1;assert(!ohci_init(devices,hosts) && !ohci_used && cmds[0]==6);
    reset();stuck_reset=1;assert(!ohci_init(devices,hosts) && !(cmds[0]&4));
    reset();map_fail=1;assert(!ohci_init(devices,hosts));
    reset();bars[0]|=1;assert(!ohci_init(devices,hosts));
    reset();bars[0]|=4;assert(!ohci_init(devices,hosts));
    reset();regs[0][0]=0x11;assert(!ohci_init(devices,hosts));
    reset();regs[0][0x48/4]=0;assert(!ohci_init(devices,hosts));
    reset();regs[0][0x48/4]=16;assert(!ohci_init(devices,hosts));
    reset();d=ready(0);regs[0][0x54/4]|=1u<<9;assert(!ohci_reset_port(hosts,0));
    puts("PASS OHCI: invalid BAR/revision/root hub/mapping, stuck firmware/reset/frame clock, low-speed storage refused");
}
