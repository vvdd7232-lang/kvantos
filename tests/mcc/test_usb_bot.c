#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_bot.h"
static unsigned step,transfers,resets,mode,payload,in;
static uint32_t tag;
static void le(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;i++) p[i]=n>>(8*i); }
static uint32_t get(const uint8_t *p) { return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static int xfer(void *ctx,int input,void *buf,unsigned size) {
    assert(ctx==&step);transfers++;
    uint8_t *p=buf;
    if(!step++) {
        assert(!input && size==31 && get(p)==0x43425355 && p[13]==0 && p[14]==10);
        assert((p[12]==0x80)==!!in);tag=get(p+4);payload=get(p+8);
        if(mode==1) return 30;
        if(!payload) step++;
        return 31;
    }
    if(step==2) {
        assert(input==!!in && size==payload);
        if(mode==2) return -1;
        if(mode==3 || mode==15) return (int)size-1;
        if(mode==4) return (int)size+1;
        return (int)size;
    }
    assert(input && size==13 && step==3);
    memset(p,0,13);le(p,0x53425355);le(p+4,tag);
    if(mode==5) return 12;
    if(mode==6) p[0]^=1;
    if(mode==7) p[4]^=1;
    if(mode==8) le(p+8,payload+1);
    if(mode==9 || mode==14 || mode==15 || mode==3) le(p+8,1);
    if(mode==10) p[12]=2;
    if(mode==11) p[12]=3;
    if(mode==12 || mode==13) { p[12]=1;if(mode==13) le(p+8,payload); }
    return 13;
}
static void reset(void *ctx) { assert(ctx==&step);resets++; }
int main(void) {
    usb_bot_io_t io={&step,xfer,reset};uint8_t cdb[10]={0x2A},buf[4096]={0};
    for(in=0;in<=1;in++) for(mode=0;mode<=15;mode++) {
        step=transfers=resets=0;
        int result=usb_bot_command(&io,0xdeadbeef,cdb,10,(int)in,buf,512);
        int expected=-1;
        if(!mode) expected=512;
        if(mode==12) expected=-2;
        if(mode==13 && !in) expected=-2;
        if((mode==3 || mode==15) && in) expected=511;
        assert(result==expected);
        assert(resets==(unsigned)(expected==-1));
        assert(transfers<=3); /* No WRITE replay. */
    }
    mode=in=0;step=transfers=resets=0;
    cdb[0]=0x35;assert(usb_bot_command(&io,1,cdb,10,0,0,0)==0 && transfers==2 && !resets);
    step=transfers=resets=0;
    assert(usb_bot_command(&io,1,cdb,17,1,buf,512)==-1);
    assert(usb_bot_command(&io,1,cdb,0,1,buf,512)==-1);
    assert(usb_bot_command(&io,1,cdb,10,0,buf,4097)==-1);
    assert(usb_bot_command(&io,1,cdb,10,0,0,1)==-1 && !transfers && !resets);
    puts("USB BOT: IN/OUT/no-data, CDB/CBW/CSW, short data, residue/tag/signature/status/phase faults, no replay PASS");
}
