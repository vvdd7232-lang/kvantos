/* Real RTC cache code, with counted CMOS ports/IRQ guards; no device access. */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define _KVANT_KERNEL_H
 typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef unsigned long kv_flags_t;
typedef struct {u8 sec,min,hour,day,month;u16 year;} rtc_time_t;
static unsigned irq_depth,io_calls;static u8 selected,registers[128];
static kv_flags_t irq_save(void) {return irq_depth++;}
static void irq_restore(kv_flags_t flags) {assert(irq_depth==flags+1);irq_depth=(unsigned)flags;}
static void outb(unsigned port,u8 value) {
    io_calls++;if(port==0x70)selected=value&127;else {assert(port==0x71);registers[selected]=value;}
}
static u8 inb(unsigned port) {io_calls++;assert(port==0x71);return registers[selected];}
#include "../../kernel/rtc.c"
int main(void) {
    u32 hms[3]={77,88,99};rtc_time_t t;
    assert(rtc_cached_clock(NULL)<0 && rtc_cached_clock(hms)<0 && !io_calls);
    assert(hms[0]==77 && hms[1]==88 && hms[2]==99);
    registers[0x0b]=6;registers[0]=58;registers[2]=59;registers[4]=23;
    registers[7]=31;registers[8]=12;registers[9]=26;
    rtc_read(&t);assert(!irq_depth && t.hour==23 && t.min==59 && t.sec==58);
    unsigned before=io_calls;registers[4]=4;irq_depth=3;
    for(unsigned i=0;i<10000;i++)assert(!rtc_cached_clock(hms) && hms[0]==23 && hms[1]==59 && hms[2]==58 && irq_depth==3);
    assert(io_calls==before);irq_depth=0;
    registers[4]=30;rtc_read(&t);assert(rtc_cached_clock(hms)<0 && hms[0]==23);
    registers[4]=12;rtc_read(&t);assert(!rtc_cached_clock(hms) && hms[0]==12);
    assert(rtc_write_time(24,0,0)<0 && !rtc_cached_clock(hms));
    assert(!rtc_write_time(10,20,30) && rtc_cached_clock(hms)<0);
    rtc_read(&t);assert(!rtc_cached_clock(hms) && hms[0]==10 && hms[1]==20 && hms[2]==30);
    assert(!rtc_write_date(2026,10,17) && rtc_cached_clock(hms)<0);
    rtc_read(&t);assert(!rtc_cached_clock(hms));
    registers[0x0b]=2;registers[0]=0x58;registers[2]=0x59;registers[4]=0x23;
    registers[7]=0x31;registers[8]=0x12;registers[9]=0x26;
    rtc_read(&t);assert(!rtc_cached_clock(hms) && hms[0]==23 && hms[1]==59 && hms[2]==58);
    for(unsigned binary=0;binary<2;binary++)for(unsigned pm=0;pm<2;pm++)for(unsigned hour=1;hour<=12;hour++) {
        registers[0x0b]=(u8)(binary?4:0);registers[0]=registers[2]=0;
        registers[4]=(u8)((binary?hour:(hour/10)*16+hour%10)|(pm?0x80:0));
        rtc_read(&t);assert(t.hour==hour%12+pm*12 && !rtc_cached_clock(hms) && hms[0]==t.hour);
    }
    registers[4]=0;rtc_read(&t);assert(rtc_cached_clock(hms)<0);
    registers[4]=13;rtc_read(&t);assert(rtc_cached_clock(hms)<0);
    registers[0x0b]=6;registers[4]=5;registers[0x0a]=0x80;
    rtc_read(&t);assert(rtc_cached_clock(hms)<0); /* Exhausted UIP guard never publishes. */
    registers[0x0a]=0;rtc_read(&t);assert(!rtc_cached_clock(hms) && hms[0]==5);
    before=io_calls;assert(rtc_cached_clock(NULL)<0 && io_calls==before && !irq_depth);
    puts("PASS RTC cache binary/BCD/12h midnight-noon, UIP guard, invalidation/ranges, 10000 reads with zero CMOS I/O, IRQ state retained");
    return 0;
}
