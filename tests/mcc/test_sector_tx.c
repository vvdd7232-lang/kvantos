/* Volatile write cache + lost power, not just pwrite()/process termination. */
#include "sector_tx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define N 160
static unsigned char stable[N][512],cache[N][512],initial[N][512];
static sector_tx_t tx,reboot;
static jmp_buf power;
static int steps,cut,edge,policy,armed,fail_at;
static unsigned char dirty[N];
static int rd(void *c,uint32_t l,void *b) { (void)c;if(l>=N) return -1;memcpy(b,cache[l],512);return 0; }
static void event(int after) {
    if(!armed) return;
    if(!after) steps++;
    if(steps==cut && after==edge) longjmp(power,1);
}
static int wr(void *c,uint32_t l,const void *b) {
    (void)c;assert(l<N);event(0);
    memcpy(cache[l],b,512);dirty[l]=1;
    /* Some writes can reach media before a barrier, in arbitrary order. */
    if(policy==1 || (policy==2 && ((l+steps)%3)==0)) memcpy(stable[l],b,512);
    event(1);return 0;
}
static int sync_io(void *c) {
    (void)c;event(0);
    if(armed && steps==fail_at) return -1;
    for(unsigned i=0;i<N;i++) if(dirty[i]) memcpy(stable[i],cache[i],512);
    memset(dirty,0,sizeof(dirty));event(1);return 0;
}
static stx_io_t io={NULL,rd,wr,sync_io,1,64,100,49,0x12345678};
static void reset(void) {
    memcpy(stable,initial,sizeof(stable));memcpy(cache,stable,sizeof(cache));
    memset(dirty,0,sizeof(dirty));memset(&tx,0,sizeof(tx));memset(&reboot,0,sizeof(reboot));
    armed=0;steps=0;fail_at=0;
}
static int operation(void) {
    unsigned char a[512],b[512];memset(a,0xA1,512);memset(b,0xB2,512);
    if(stx_begin(&tx,&io)<0 || stx_write(&tx,2,a)<0 || stx_write(&tx,7,b)<0 || stx_write(&tx,2,b)<0) return -1;
    return stx_commit(&tx);
}
static void cold(void) {
    armed=0;memcpy(cache,stable,sizeof(cache));memset(dirty,0,sizeof(dirty));
    assert(stx_recover(&reboot,&io)==0);
    int old=stable[2][0]==initial[2][0] && stable[7][0]==initial[7][0];
    int new_=stable[2][0]==0xB2 && stable[7][0]==0xB2;
    assert(old || new_);
    for(unsigned i=0;i<512;i++) {
        assert(stable[2][i]==stable[2][0]);assert(stable[7][i]==stable[7][0]);
    }
    for(unsigned i=0;i<N;i++) if(i!=2 && i!=7 && !(i>=100 && i<149)) assert(!memcmp(stable[i],initial[i],512));
}
int main(void) {
    for(unsigned i=1;i<65;i++) memset(cache[i],(int)i,512);
    memcpy(stable,cache,sizeof(stable));
    assert(stx_format(&tx,&io)==0);memcpy(initial,stable,sizeof(initial));
    reset();armed=1;assert(operation()==0);armed=0;int max=steps;
    for(policy=0;policy<3;policy++) for(edge=0;edge<2;edge++) for(cut=1;cut<=max;cut++) {
        reset();armed=1;if(!setjmp(power)) { operation();assert(0); } cold();
    }
    printf("PASS %d cache-loss cuts (3 persistence policies, before/after every write/barrier)\n",max*6);
    /* Commit-close flush can fail AFTER a clean header has reached media.
       Rolling metadata back now could corrupt a clean, already committed FS. */
    policy=1;
    for(int at=1;at<=max;at++) {
        reset();fail_at=at;armed=1;int r=operation();armed=0;
        if(r<0 && tx.closing) {
            unsigned char snapshot[N][512];memcpy(snapshot,stable,sizeof(snapshot));
            assert(stx_abort(&tx)<0);assert(!memcmp(stable,snapshot,sizeof(snapshot)));
        } else if(r<0 && tx.active) stx_abort(&tx);
        cold();
    }
    puts("PASS ambiguous commit-close failure never attempts unsafe RAM rollback");
    /* Interrupted recovery must be repeatable, including after its close. */
    reset();assert(stx_begin(&tx,&io)==0);
    unsigned char value[512];memset(value,0xA1,512);
    assert(stx_write(&tx,2,value)==0);assert(stx_write(&tx,7,value)==0);assert(sync_io(NULL)==0);
    static unsigned char pending[N][512];memcpy(pending,stable,sizeof(pending));
    for(policy=0;policy<3;policy++) for(edge=0;edge<2;edge++) for(cut=1;cut<=5;cut++) {
        memcpy(stable,pending,sizeof(pending));memcpy(cache,stable,sizeof(cache));memset(dirty,0,sizeof(dirty));steps=0;
        armed=1;if(!setjmp(power)) { stx_recover(&reboot,&io);assert(0); }
        cold();assert(stable[2][0]==2 && stable[7][0]==7);
    }
    puts("PASS 30 recovery interruptions: replay is idempotent after another power loss");
    reset();assert(stx_begin(&tx,&io)==0);
    unsigned char b[512]={0};for(unsigned i=1;i<=STX_MAX;i++) assert(stx_write(&tx,i,b)==0);
    assert(stx_write(&tx,STX_MAX+1,b)<0);assert(stx_commit(&tx)<0);assert(stx_abort(&tx)==0);cold();
    puts("PASS bounded log exhaustion restores every recorded sector");
    reset();assert(stx_begin(&tx,&io)==0);assert(stx_write(&tx,0,b)<0);assert(stx_commit(&tx)<0);assert(stx_abort(&tx)==0);
    stx_io_t invalid=io;invalid.journal=20;assert(stx_format(&tx,&invalid)<0);
    assert(stx_format(&tx,&io)<0);
    puts("PASS bounds, overlap and nonempty-journal provisioning refusal");
    reset();assert(stx_begin(&tx,&io)==0);assert(stx_write(&tx,2,b)==0);
    stable[101][20]^=1;memcpy(cache,stable,sizeof(cache));
    unsigned char before[64][512];memcpy(before,stable,sizeof(before));
    assert(stx_recover(&reboot,&io)<0);assert(!memcmp(before,stable,sizeof(before)));
    puts("PASS bad backup CRC refuses all rollback writes");
    return 0;
}
