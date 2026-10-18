/* Real driver with a deterministic register/DMA model; not physical proof. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"
static u32 model_read(volatile u32 *p);
static void model_write(volatile u32 *p,u32 v);
static volatile u32 *model_mmio(u32 base);
static int model_dma(void *p,u32 n,u32 *out);
static void progress(void);
#define AHCI_RD(p) model_read(p)
#define AHCI_WR(p,v) model_write(p,v)
#define AHCI_MMIO(base) model_mmio(base)
#define AHCI_PAUSE() progress()
#define AHCI_DMA(p,n,out) model_dma(p,n,out)
#include "../../kernel/ahci.c"

static u32 registers[2][0x1100/4],pci_commands[2];
static pci_dev_t devices[2];
static int controllers,mode,short_identify,long_sector,no_lba48,stuck_bios,stuck_cr,stuck_fr,missing_dma;
static int operations,reads,clb_writes,phys_gap,supported_flush,writes,flushes,link_race;
static u32 clock_tick,capacity;
static struct { void *ptr;u32 address,bytes; } mappings[40];
static unsigned map_count;
enum { GOOD, TASK_ERROR, SHORT_READ, TIMEOUT, UNPLUG, BUS_ERROR, INTERFACE_ERROR, DEVICE_FAULT, LATE_ERROR, STOP_STUCK };
u64 timer_ticks(void) { return clock_tick; }
u32 timer_hz(void) { return 500; }
static void progress(void) { clock_tick++; }
u32 paging_phys(u32 v) { return v>=0x1000 && v<0x10000?v+(phys_gap && v>=0x2000?4096:0):0; }
int paging_map_mmio(u32 base,u32 size) { assert(size==0x1100);return base>=0xF0000000?0:-1; }
u32 pci_count(void) { return (u32)controllers; }
pci_dev_t *pci_get(u32 i) { return i<(u32)controllers?devices+i:NULL; }
u32 pci_read32(u8 b,u8 s,u8 f,u8 off) { (void)b;(void)f;assert(off==4);return pci_commands[s]; }
void pci_write32(u8 b,u8 s,u8 f,u8 off,u32 val) { (void)b;(void)f;assert(off==4 && !(val&0xFFFF0000));
    if((val&4) && !(pci_commands[s]&4))
        for(int p=0;p<6;p++) assert(!(registers[s][0x40+p*32+CMD]&(ST|FRE|CR|FR)));
    pci_commands[s]=val; }
static volatile u32 *model_mmio(u32 base) {
    for(int i=0;i<controllers;i++) if(base==(devices[i].bar[5]&~15u)) return registers[i];
    assert(0);return NULL;
}
static int model_dma(void *p,u32 n,u32 *out) {
    if(missing_dma) return -1;
    for(unsigned i=0;i<map_count;i++) if(mappings[i].ptr==p) {
        if(n>mappings[i].bytes) mappings[i].bytes=n;
        *out=mappings[i].address;return 0;
    }
    assert(map_count<40);
    unsigned i=map_count++;
    mappings[i].ptr=p;mappings[i].bytes=n;
    mappings[i].address=0x01000000+i*0x10000+((kv_addr_t)p&4095);
    *out=mappings[i].address;return 0;
}
static void *dma_ptr(u32 address,u32 bytes) {
    for(unsigned i=0;i<map_count;i++) if(mappings[i].address==address) {
        assert(bytes<=mappings[i].bytes);return mappings[i].ptr;
    }
    assert(0);return NULL;
}
static u8 pattern(u32 sector,u32 byte) { return (u8)(sector*7+byte*13); }
static void issue(volatile u32 *r) {
    assert((r[CMD]&(ST|FRE))==(ST|FRE));
    assert(!r[CLB+1] && !r[FB+1] && !r[IE]);
    ahci_header_t *h=dma_ptr(r[CLB],1024);
    assert(!h->table_hi && !h->bytes);
    ahci_table_t *t=dma_ptr(h->table,sizeof(*t));
    assert(!t->prdt.hi && !t->prdt.reserved && t->fis[0]==0x27 && t->fis[1]==0x80);
    u8 opcode=t->fis[2],n=t->fis[12];
    int flush=opcode==0xEA,write=opcode==0x35;
    assert((opcode==0x25 || opcode==0xEC || write || flush) && (flush?!n:(n && n<=64)));
    assert(h->flags==(5|(write?64:0)) && h->prdtl==(flush?0:1));
    assert(!t->fis[9] && !t->fis[10] && !t->fis[13] && t->fis[7]==0x40);
    u32 bytes=(u32)n*512;
    if(flush) assert(!t->prdt.bytes && !t->prdt.address);
    else assert(t->prdt.bytes==bytes-1);
    u8 *data=flush?NULL:dma_ptr(t->prdt.address,bytes);operations++;
    if(write) writes++;
    if(flush) flushes++;
    if(opcode==0xEC) {
        assert(n==1);memset(data,0,512);u16 *id=(u16 *)data;
        id[49]=1u<<9;id[83]=no_lba48?0:0x4400|(supported_flush?0x2000:0);
        id[100]=(u16)capacity;id[101]=(u16)(capacity>>16);
        id[106]=long_sector?0x5000:0x4000;id[117]=long_sector?2048:256;
        const char model[41]="MODEL SATA TEST                         ";
        for(int i=0;i<20;i++) id[27+i]=((u16)(u8)model[2*i]<<8)|(u8)model[2*i+1];
        h->bytes=short_identify?0:512;r[CI]=0;return;
    }
    if(opcode==0x25) reads++;
    u32 lba=(u32)t->fis[4]|((u32)t->fis[5]<<8)|((u32)t->fis[6]<<16)|((u32)t->fis[8]<<24);
    assert(flush || (lba<capacity && n<=capacity-lba));
    if(mode==TIMEOUT || mode==STOP_STUCK) { if(mode==STOP_STUCK) stuck_cr=1;return; }
    if(mode==UNPLUG) { r[SSTS]=0;return; }
    if(mode==TASK_ERROR) { r[IS]|=1u<<30;return; }
    if(mode==BUS_ERROR) { r[IS]|=1u<<29;return; }
    if(mode==INTERFACE_ERROR) { r[IS]|=1u<<27;return; }
    if(mode==DEVICE_FAULT) r[TFD]=0x20;
    if(mode==LATE_ERROR) r[IS]|=1u<<30;
    for(u32 at=0;at<bytes;at++) {
        if(write) assert(data[at]==pattern(lba+at/512,at%512));
        else data[at]=pattern(lba+at/512,at%512);
    }
    h->bytes=mode==SHORT_READ?(bytes?bytes-1:1):bytes;r[CI]=0;
}
static u32 model_read(volatile u32 *p) { return *p; }
static void model_write(volatile u32 *p,u32 v) {
    for(int i=0;i<controllers;i++) {
        volatile u32 *h=registers[i];
        if(p==h+BOHC) { *p=stuck_bios?v:(v&~17u);return; }
        for(int j=0;j<6;j++) {
            volatile u32 *r=h+0x40+j*32;
            if(p==r+IS || p==r+SERR) {
                *p&=~v;
                if(p==r+IS && link_race && v==~IO_ERRORS) { *p|=1u<<22;link_race=0; }
                return;
            }
            if(p==r+CMD) {
                v=(v&~(CR|FR))|((v&ST)?CR:(stuck_cr?CR:0))|((v&FRE)?FR:(stuck_fr?FR:0));
                *p=v;return;
            }
            if(p==r+CLB) clb_writes++;
            if(p==r+CI) { *p=v;assert(v==1);issue(r);return; }
        }
    }
    *p=v;
}
static void reset(void) {
    memset(ahci_ports,0,sizeof(ahci_ports));memset(ahci_drive_port,0,sizeof(ahci_drive_port));
    ahci_port_count=ahci_drive_count=ahci_initialized=0;
    memset(registers,0,sizeof(registers));memset(devices,0,sizeof(devices));memset(pci_commands,0,sizeof(pci_commands));
    controllers=1;mode=GOOD;short_identify=long_sector=no_lba48=stuck_bios=stuck_cr=stuck_fr=missing_dma=0;
    operations=reads=clb_writes=phys_gap=supported_flush=writes=flushes=link_race=0;clock_tick=0;capacity=0x23456789;map_count=0;
    for(int i=0;i<2;i++) {
        devices[i].slot=(u8)i;devices[i].class_code=1;devices[i].subclass=6;devices[i].prog_if=1;
        devices[i].bar[5]=0xF0000000+i*0x2000;registers[i][0]=5;pci_commands[i]=6;
        registers[i][CAP2]=1;registers[i][BOHC]=17;registers[i][PI]=1u|(1u<<(i?5:2));
        u32 *cd=registers[i]+0x40;cd[SSTS]=0x123;cd[SIG]=0xEB140101;cd[CMD]=ST|FRE|CR|FR;
        u32 *r=registers[i]+0x40+(i?5:2)*32;
        r[SSTS]=0x123;r[SIG]=0x101;r[CMD]=ST|FRE|CR|FR;
    }
}
static void data_ok(const u8 *p,u32 lba,u32 n) {
    for(u32 at=0;at<n*512;at++) assert(p[at]==pattern(lba+at/512,at%512));
}
int main(void) {
    u32 out;reset();
    assert(ahci_dma_address((void *)0x1800,0x2800,&out)==0 && out==0x1800);
    phys_gap=1;assert(ahci_dma_address((void *)0x1800,0x2800,&out)<0);phys_gap=0;
    assert(ahci_dma_address(NULL,512,&out)<0 && ahci_dma_address((void *)0x1000,0,&out)<0);
    assert(ahci_dma_address((void *)(kv_addr_t)0xFFFFFFF0u,512,&out)<0);
    assert(ahci_dma_address((void *)((kv_addr_t)1<<32),512,&out)<0);
    reset();controllers=2;ahci_init();assert(ahci_count()==2 && ahci_sectors(0)==capacity && ahci_sectors(1)==capacity);
    assert(!strcmp(ahci_model(0),"MODEL SATA TEST") && strstr(ahci_status(0),"read-only"));
    static u8 data[255*512+2];memset(data,0xA5,sizeof(data));
    assert(ahci_read(1,0x12345678,255,data+1)==0 && reads==4);data_ok(data+1,0x12345678,255);
    assert(data[0]==0xA5 && data[sizeof(data)-1]==0xA5);
    int old=operations;ahci_init();assert(operations==old); /* idempotent */
    assert(ahci_read(-1,0,1,data)<0 && ahci_read(2,0,1,data)<0 && ahci_read(0,0,0,data)<0);
    assert(ahci_read(0,capacity,1,data)<0 && ahci_read(0,capacity-1,2,data)<0 && ahci_read(0,0,1,NULL)<0);
    assert(operations==old && ahci_sectors(0)==capacity);
    ahci_ports[ahci_drive_port[0]].busy=1;assert(ahci_read(0,0,1,data)<0 && operations==old);ahci_ports[ahci_drive_port[0]].busy=0;
    assert(ahci_read(0,capacity-1,1,data)==0);data_ok(data,capacity-1,1);
    puts("PASS AHCI: two controllers/ports, DMA/FIS/PRDT, unaligned 255-sector read, bounds and busy guard");
    for(int fault=TASK_ERROR;fault<=STOP_STUCK;fault++) {
        reset();ahci_init();mode=fault;memset(data,0xA5,512);
        ahci_port_t *p=&ahci_ports[ahci_drive_port[0]];u32 addr=p->headers[0].table;
        assert(ahci_read(0,1,1,data)<0 && p->fenced && !p->busy && !ahci_sectors(0));
        for(int i=0;i<512;i++) assert(data[i]==0xA5);
        old=operations;ahci_init();assert(ahci_read(0,1,1,data)<0 && operations==old);
        assert(p->headers[0].table==addr && ahci_port_count==1 && ahci_count()==1);
    }
    puts("PASS AHCI: TFES/HBFS/interface/DF/short DMA/timeout/link loss/stuck CR -> fence; no DMA reuse");
    reset();ahci_init();ahci_ports[0].regs[TFD]=0x80;old=operations;
    assert(ahci_read(0,0,1,data)<0 && operations==old);
    reset();ahci_init();ahci_ports[0].regs[IS]=1u<<22;old=operations;
    assert(ahci_read(0,0,1,data)<0 && operations==old && !ahci_sectors(0));
    for(int fault=0;fault<12;fault++) {
        reset();u32 *r=registers[0]+0x40+2*32;
        switch(fault) {
        case 0: devices[0].prog_if=0;break;
        case 1: devices[0].bar[5]|=1;break;
        case 2: devices[0].bar[5]|=4;break;
        case 3: devices[0].bar[5]=0x1000;break;
        case 4: r[SIG]=0xEB140101;break;
        case 5: r[SSTS]=0;break;
        case 6: stuck_bios=1;break;
        case 7: stuck_cr=1;break;
        case 8: stuck_fr=1;break;
        case 9: short_identify=1;break;
        case 10: long_sector=1;break;
        case 11: no_lba48=1;break;
        }
        ahci_init();assert(!ahci_count());
        if(fault<=8) assert(!clb_writes && !operations);
    }
    reset();missing_dma=1;ahci_init();assert(!ahci_count() && !clb_writes && !operations);
    reset();capacity=0;ahci_init();assert(!ahci_count());
    reset();capacity=0xFFFFFFFFu;ahci_init();assert(ahci_read(0,0xFFFFFFFEu,1,data)==0);
    assert(ahci_read(0,0xFFFFFFFFu,1,data)<0 && ahci_read(0,0xFFFFFFFEu,2,data)<0);
    reset();controllers=2;
    for(int c=0;c<2;c++) for(int port=0;port<6;port++) {
        registers[c][PI]|=1u<<port;
        u32 *r=registers[c]+0x40+port*32;r[SSTS]=0x123;r[SIG]=0x101;
    }
    ahci_init();assert(ahci_count()==8 && ahci_port_count==8);
    assert(ahci_read(7,0,1,data)==0);
    puts("PASS AHCI: bounded eight-port pool; all firmware/ATAPI engines retired before bus master");
    /* Ordinary, ungranted SATA must never emit a WRITE or FLUSH opcode. */
    reset();supported_flush=1;ahci_init();old=operations;
    u32 generation=ahci_generation(0);
    assert(generation && ahci_write(0,1,1,data)<0 && ahci_sync(0)<0 && operations==old);
    assert(ahci_fat_authorize(0,0x12345678,255,generation)==0);
    for(u32 n=0;n<255*512;n++) data[n+1]=pattern(0x12345678+n/512,n%512);
    assert(ahci_write(0,0x12345678,255,data+1)==0 && writes==4 && flushes==0);
    assert(ahci_sync(0)==0 && flushes==1);
    old=operations;
    assert(ahci_write(0,0x12345677,1,data)<0 && ahci_write(0,0x12345776,2,data)<0 && operations==old);
    ahci_fat_revoke(0);assert(ahci_sync(0)<0 && ahci_write(0,0x12345678,1,data)<0);
    assert(ahci_fat_authorize(0,capacity-1,2,generation)<0);
    assert(ahci_fat_authorize(0,0,0,generation)<0 && ahci_fat_authorize(0,0,1,0)<0);
    assert(ahci_fat_authorize(0,0,1,generation+1)<0);
    assert(ahci_fat_authorize(0,1,1,generation)==0);
    ahci_rescan();assert(ahci_generation(0)!=generation && !ahci_ports[0].write_allowed);
    assert(ahci_fat_authorize(0,1,1,generation)<0);
    assert(ahci_fat_authorize(0,1,1,ahci_generation(0))==0);
    ahci_ports[0].regs[IS]|=1u<<22;old=operations;
    assert(ahci_write(0,1,1,data)<0 && operations==old && !ahci_generation(0));
    reset();supported_flush=1;ahci_init();
    assert(ahci_fat_authorize(0,1,1,ahci_generation(0))==0);link_race=1;old=operations;
    assert(ahci_write(0,1,1,data)<0 && operations==old); /* latched while clearing normal completions */
    reset();ahci_init();assert(ahci_fat_authorize(0,1,1,ahci_generation(0))<0); /* no FLUSH EXT */
    for(int sync=0;sync<2;sync++) for(int fault=TASK_ERROR;fault<=STOP_STUCK;fault++) {
        reset();supported_flush=1;ahci_init();
        assert(ahci_fat_authorize(0,1,1,ahci_generation(0))==0);
        for(int n=0;n<512;n++) data[n]=pattern(1,(u32)n);
        mode=fault;
        assert((sync?ahci_sync(0):ahci_write(0,1,1,data))<0);
        assert(ahci_ports[0].fenced && !ahci_ports[0].write_allowed && !ahci_ports[0].busy && !ahci_generation(0));
        old=operations;assert(ahci_write(0,1,1,data)<0 && ahci_sync(0)<0 && operations==old);
    }
    puts("PASS AHCI writes: 255-sector DMA OUT, zero-PRDT FLUSH EXT, bounded leases, stale generation/rescan/link-latch refusal");
    puts("PASS AHCI write + flush errors: short/TFES/HBFS/DF/interface/timeout/unplug/stuck CR; revoke/fence/no replay");
    puts("PASS AHCI: BIOS handoff/engine stop, missing DMA, ATAPI/4Kn/LBA48/invalid BAR/capacity refusals");
}
