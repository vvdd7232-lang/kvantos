/* Actual UHCI backend with simulated port I/O/frame retirement, not hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#define HC_MAX 4
#define TD_MAX 512
#define ACTIVE (1u<<23)
#define TD_ERRORS 0x007E0000u
#define TERM 1u
#define IN 0x69u
#define OUT 0xE1u
#define SETUP 0x2Du
typedef struct { volatile u32 link,status;u32 token,buffer; } td_t;
typedef struct { volatile u32 link,element;u32 pad[2]; } qh_t;
typedef struct { u16 io;u8 dead; } hc_t;
typedef struct { hc_t *hc;u32 generation;u8 address,port; } usb_disk_t;
static hc_t hcs[HC_MAX];
static qh_t queues[HC_MAX];
static u32 physical(const void *p) { assert((kv_addr_t)p<0x100000000ULL);return (u32)(kv_addr_t)p; }
static u16 io_read(u16);
static u64 clock_now(void);
static int frames_wait(hc_t *,unsigned);
static u32 port_status(hc_t *,int);
static void usb_stop(hc_t *h) { h->dead=1;queues[h-hcs].element=TERM; }
static void progress(void);
#define inw io_read
#define UHCI_PAUSE() progress()
#include "../../kernel/usb_uhci.h"
#undef inw

enum { GOOD,SHORT,STALL,TIMEOUT,STOP_CLOCK,CHANGE,DETACH,BAD_ACTUAL };
static int mode,waits,issues;
static unsigned frames[HC_MAX];
static u32 ports[HC_MAX];
static u64 ticks;
static u16 io_read(u16 p) {
    for(int i=0;i<HC_MAX;i++) if(p==hcs[i].io+6) return frames[i]++;
    for(int i=0;i<HC_MAX;i++) if(p==hcs[i].io+2) return 0;
    assert(0);return 0;
}
static u32 port_status(hc_t *h,int p) { assert(p==0);return ports[h-hcs]; }
static u64 clock_now(void) { return ++ticks; }
static int frames_wait(hc_t *h,unsigned n) { assert(n==2);assert(queues[h-hcs].element==TERM);waits++;return mode!=STOP_CLOCK; }
static void progress(void) {
    for(int h=0;h<HC_MAX;h++) if(queues[h].element!=TERM) {
        issues++;
        if(mode==TIMEOUT || mode==STOP_CLOCK) continue;
        if(mode==CHANGE || mode==DETACH) { ports[h]=mode==CHANGE?7:0;continue; }
        td_t *td=(void *)(kv_addr_t)(queues[h].element&~15u);
        assert(td==uhci_tds[h]);unsigned offset=0;
        for(unsigned i=0;i<TD_MAX;i++) {
            assert(td[i].status&ACTIVE);
            unsigned len=(((td[i].token>>21)+1)&2047),pid=td[i].token&255;
            assert(td[i].buffer==physical(uhci_data[h]+offset));
            assert(len<=64 && ((td[i].token>>8)&127)==(unsigned)h+1);
            unsigned actual=mode==SHORT?3:mode==BAD_ACTUAL?len+1:len;
            td[i].status=(actual-1)&2047;
            if(mode==STALL) td[i].status|=1u<<22;
            if(pid==IN) for(unsigned j=0;j<actual && j<len;j++) uhci_data[h][offset+j]=(u8)(offset+j+1);
            else for(unsigned j=0;j<len;j++) assert(uhci_data[h][offset+j]==(u8)(offset+j+1));
            if(mode!=GOOD || td[i].link==TERM) break;
            assert(td[i].link==(physical(&td[i+1])|4));offset+=len;
        }
    }
}
static void fresh(void) {
    memset(hcs,0,sizeof(hcs));memset(queues,0,sizeof(queues));memset(frames,0,sizeof(frames));
    for(int h=0;h<HC_MAX;h++) { hcs[h].io=0x100+32*h;ports[h]=5;queues[h].element=TERM; }
    mode=GOOD;issues=waits=0;ticks=0;
}
static usb_disk_t disk(int h) { usb_disk_t d={.hc=&hcs[h],.generation=1,.address=h+1};return d; }
int main(void) {
    fresh();usb_disk_t d=disk(0);u8 b[4096];memset(b,0xAA,sizeof(b));int toggle=0;
    assert(uhci_transfer(&d,IN,1,8,&toggle,b,19)==19 && toggle==1 && waits==1);
    for(int j=0;j<19;j++) assert(b[j]==j+1);
    assert(b[19]==0xAA);
    for(int j=0;j<4096;j++) b[j]=(u8)(j+1);
    assert(uhci_transfer(&d,OUT,2,8,&toggle,b,4096)==4096 && toggle==1);
    mode=SHORT;memset(b,0xAA,sizeof(b));toggle=0;
    assert(uhci_transfer(&d,IN,1,8,&toggle,b,32)==3 && toggle==1 && b[3]==0xAA);
    for(int j=0;j<3;j++) assert(b[j]==j+1);
    puts("UHCI packet chain, 512 TD bound, OUT, short IN/toggle, retirement PASS");
    for(int m=STALL;m<=BAD_ACTUAL;m++) {
        fresh();d=disk(0);mode=m;toggle=0;memset(b,0xAA,sizeof(b));
        assert(uhci_transfer(&d,IN,1,8,&toggle,b,32)==-1 && toggle==0 && queues[0].element==TERM);
        for(unsigned j=0;j<sizeof(b);j++) assert(b[j]==0xAA);
        assert(hcs[0].dead==(m==STOP_CLOCK));
    }
    puts("UHCI stall/timeout/root race/length/clock failure: no false copy/toggle PASS");
    fresh();d=disk(0);mode=STOP_CLOCK;toggle=0;
    assert(uhci_transfer(&d,IN,1,8,&toggle,b,32)==-1 && hcs[0].dead);
    td_t *old=uhci_tds[0];u8 *late=uhci_data[0];
    mode=GOOD;usb_disk_t other=disk(1);toggle=0;assert(uhci_transfer(&other,IN,1,8,&toggle,b,32)==32);
    old->status=0;memset(late,0xFE,4096);
    for(int j=0;j<32;j++) assert(b[j]==j+1 && uhci_data[1][j]==j+1);
    int n=issues;assert(uhci_transfer(&d,IN,1,8,&toggle,b,32)==-1 && issues==n);
    fresh();d=disk(0);toggle=0;
    assert(uhci_transfer(&d,IN,1,0,&toggle,b,1)==-1);
    assert(uhci_transfer(&d,IN,1,7,&toggle,b,1)==-1);
    assert(uhci_transfer(&d,IN,16,8,&toggle,b,1)==-1);
    assert(uhci_transfer(&d,IN,1,8,NULL,b,1)==-1);
    assert(uhci_transfer(&d,IN,1,8,&toggle,NULL,1)==-1);
    assert(uhci_transfer(&d,IN,1,8,&toggle,b,4097)==-1);
    assert(!issues);
    puts("UHCI private DMA/late completion/fenced-slot isolation and input bounds PASS");
    return 0;
}
