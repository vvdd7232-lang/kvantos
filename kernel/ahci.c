/* M3.2: polled SATA DMA; writes/FLUSH EXT require a bounded FAT operation lease.
 * One slot and a private, pinned DMA bounce buffer per attempted port. An I/O
 * fault fences the port until reboot; even a failed engine stop NEVER frees or
 * reuses its DMA memory. No NCQ, ATAPI, port multiplier, hotplug or installer.
 * x86 cache-coherent DMA, physical addresses below 4 GiB; no IOMMU support. */
#include "kernel.h"

#define AHCI_PORTS 8
#define GHC 1
#define PI 3
#define CAP2 9
#define BOHC 10
#define CLB 0
#define FB 2
#define IS 4
#define IE 5
#define CMD 6
#define TFD 8
#define SIG 9
#define SSTS 10
#define SERR 12
#define SACT 13
#define CI 14
#define ST 1u
#define FRE (1u<<4)
#define FR (1u<<14)
#define CR (1u<<15)
#define IO_ERRORS (0x7D000000u | (1u<<6) | (1u<<22))
#define DMA_BYTES (64u*512)

/* The host test replaces only MMIO/address translation/poll progress. It runs
 * this same command construction, enumeration, completion and fencing code. */
#ifndef AHCI_RD
#define AHCI_RD(p) (*(p))
#define AHCI_WR(p,v) (*(p)=(v))
#define AHCI_MMIO(base) ((volatile u32 *)(kv_addr_t)(base))
#define AHCI_PAUSE() __asm__ volatile("pause")
#endif

typedef struct {
    u16 flags,prdtl;
    volatile u32 bytes;
    u32 table,table_hi,reserved[4];
} ahci_header_t;
typedef struct { u32 address,hi,reserved,bytes; } ahci_prdt_t;
typedef struct {
    u8 fis[64],atapi[16],reserved[48];
    ahci_prdt_t prdt;
} ahci_table_t;
typedef struct {
    ahci_header_t headers[32] __attribute__((aligned(1024)));
    u8 received[256] __attribute__((aligned(256)));
    ahci_table_t table __attribute__((aligned(128)));
    u8 data[DMA_BYTES] __attribute__((aligned(4096)));
    volatile u32 *regs;
    u32 sectors,number,generation,write_start,write_count;
    int fenced,busy,flush_ext,write_allowed;
    char model[41];
} ahci_port_t;
_Static_assert(sizeof(ahci_header_t)==32,"AHCI command header");
_Static_assert(sizeof(ahci_table_t)==144,"AHCI command table + one PRDT");
static ahci_port_t ahci_ports[AHCI_PORTS];
static int ahci_drive_port[AHCI_PORTS],ahci_port_count,ahci_drive_count,ahci_initialized;
static u32 ahci_epoch;

static int ahci_dma_address(void *ptr,u32 bytes,u32 *out) {
    kv_addr_t address=(kv_addr_t)ptr;
    if(!bytes || address>0xFFFFFFFFu || bytes-1>0xFFFFFFFFu-address) return -1;
    u32 base=paging_phys((u32)address);
    if(!base || bytes-1>0xFFFFFFFFu-base) return -1;
    for(u32 off=0;off<bytes;) {
        if(paging_phys((u32)address+off)!=base+off) return -1;
        u32 step=4096-(((u32)address+off)&4095);
        off+=step<bytes-off?step:bytes-off;
    }
    *out=base;return 0;
}
#ifndef AHCI_DMA
#define AHCI_DMA(p,n,out) ahci_dma_address(p,n,out)
#endif
static int ahci_wait_clear(volatile u32 *reg,u32 bits,u32 ms) {
    u32 start=(u32)timer_ticks(),ticks=(timer_hz()*ms+999)/1000;
    for(u32 left=2000000;left;left--) {
        if(!(AHCI_RD(reg)&bits)) return 0;
        if((u32)((u32)timer_ticks()-start)>=ticks) break;
        AHCI_PAUSE();
    }
    return -1; /* iteration budget also bounds waits with interrupts disabled */
}
static int ahci_stop(volatile u32 *r) {
    AHCI_WR(r+CMD,AHCI_RD(r+CMD)&~ST);
    if(ahci_wait_clear(r+CMD,CR,500)<0) return -1;
    AHCI_WR(r+CMD,AHCI_RD(r+CMD)&~FRE);
    return ahci_wait_clear(r+CMD,FR,500);
}
static int ahci_fail(ahci_port_t *p) {
    p->fenced=1;p->write_allowed=0;p->generation=0;
    AHCI_WR(p->regs+IE,0);
    (void)ahci_stop(p->regs);
    return -1;
}
static int ahci_command(ahci_port_t *p,u8 opcode,u32 lba,u8 count) {
    volatile u32 *r=p->regs;
    int flush=opcode==0xEA,write=opcode==0x35;
    if(p->fenced || (opcode!=0xEC && opcode!=0x25 && !write && !flush) ||
       (flush?(count || lba):(!count || count>64))) return -1;
    if((write || flush) && (!p->write_allowed || !p->flush_ext)) return -1;
    if(write && (lba<p->write_start || lba-p->write_start>=p->write_count ||
       count>p->write_count-(lba-p->write_start))) return -1;
    if((AHCI_RD(r+SSTS)&0xF)!=3 || (AHCI_RD(r+IS)&IO_ERRORS) || AHCI_RD(r+CI) || AHCI_RD(r+SACT) ||
       ahci_wait_clear(r+TFD,0x88,1000)<0) return ahci_fail(p);
    memset(p->headers,0,sizeof(p->headers));
    memset(&p->table,0,sizeof(p->table));
    u32 table,data=0;
    if(AHCI_DMA(&p->table,sizeof(p->table),&table)<0 || (table&127) ||
       (!flush && (AHCI_DMA(p->data,(u32)count*512,&data)<0 || (data&1)))) return ahci_fail(p);
    p->headers[0].flags=5|(write?(1u<<6):0); /* 20-byte H2D FIS + direction */
    p->headers[0].prdtl=flush?0:1;p->headers[0].table=table;
    if(!flush) { p->table.prdt.address=data;p->table.prdt.bytes=(u32)count*512-1; }
    u8 *f=p->table.fis;
    f[0]=0x27;f[1]=0x80;f[2]=opcode;f[7]=0x40;
    f[4]=(u8)lba;f[5]=(u8)(lba>>8);f[6]=(u8)(lba>>16);f[8]=(u8)(lba>>24);
    f[12]=count;
    /* Never erase an asynchronous link/PHY/error latch between lease check
     * and issue. Clearing only normal completions preserves replacement evidence. */
    AHCI_WR(r+IS,~IO_ERRORS);
    __sync_synchronize();
    if((AHCI_RD(r+SSTS)&15)!=3 || (AHCI_RD(r+IS)&IO_ERRORS)) return ahci_fail(p);
    AHCI_WR(r+CI,1);
    u32 start=(u32)timer_ticks(),ticks=timer_hz()*2;
    int complete=0;
    for(u32 left=2000000;left;left--) {
        if((AHCI_RD(r+SSTS)&15)!=3 || (AHCI_RD(r+IS)&IO_ERRORS)) break;
        if(!(AHCI_RD(r+CI)&1)) { complete=1;break; }
        if((u32)((u32)timer_ticks()-start)>=ticks) break;
        AHCI_PAUSE();
    }
    __sync_synchronize();
    if(!complete || (AHCI_RD(r+IS)&IO_ERRORS) || (AHCI_RD(r+TFD)&0xA1) ||
       p->headers[0].bytes!=(u32)count*512) return ahci_fail(p);
    return 0;
}
static int ahci_identify(ahci_port_t *p) {
    if(ahci_command(p,0xEC,0,1)<0) return -1;
    const u16 *id=(const u16 *)p->data;
    if((id[0]&0x8000) || !(id[49]&(1u<<9)) || (id[83]&0xC400)!=0x4400) return ahci_fail(p);
    if((id[106]&0xD000)==0x5000 && ((u32)id[117]|((u32)id[118]<<16))!=256) return ahci_fail(p);
    u64 n=(u64)id[100]|((u64)id[101]<<16)|((u64)id[102]<<32)|((u64)id[103]<<48);
    if(!n || n>0xFFFFFFFFu) return ahci_fail(p); /* shared block ABI is u32 LBA */
    p->sectors=(u32)n;p->flush_ext=!!(id[83]&(1u<<13));
    if(!++ahci_epoch) ++ahci_epoch;
    p->generation=ahci_epoch;
    for(int i=0;i<20;i++) { p->model[i*2]=(char)(id[27+i]>>8);p->model[i*2+1]=(char)id[27+i]; }
    p->model[40]=0;
    for(int n=40;n && p->model[n-1]==' ';n--) p->model[n-1]=0;
    if(!p->model[0]) strcpy(p->model,"SATA disk");
    return 0;
}
void ahci_init(void) {
    if(ahci_initialized) return; /* no re-use of quarantined DMA buffers */
    ahci_initialized=1;
    for(u32 i=0;i<pci_count();i++) {
        pci_dev_t *dev=pci_get(i);
        if(!dev || dev->class_code!=1 || dev->subclass!=6 || dev->prog_if!=1 || (dev->header_type&127)) continue;
        u32 bar=dev->bar[5],base=bar&~15u;
        if((bar&15) || base<0x08000000u || base>0xFFFFD000u) continue;
        /* AHCI register window: 0x100 host bytes + up to 32 x 0x80 ports. */
        if(paging_map_mmio(base,0x1100)<0) continue;
        u32 pci_cmd=pci_read32(dev->bus,dev->slot,dev->func,4)&0xFFFF;
        pci_write32(dev->bus,dev->slot,dev->func,4,pci_cmd|2); /* no status W1C echo */
        volatile u32 *h=AHCI_MMIO(base);
        if(AHCI_RD(h+CAP2)&1) {
            AHCI_WR(h+BOHC,(AHCI_RD(h+BOHC)|2u)&~8u);
            if(ahci_wait_clear(h+BOHC,17,2000)<0) continue;
        }
        /* After BIOS handoff, block DMA before retiring ALL firmware engines,
         * including unsupported ATAPI ports. Never re-enable bus mastering
         * while any engine can still use firmware-owned CLB/FB memory. */
        pci_write32(dev->bus,dev->slot,dev->func,4,(pci_cmd|2)&~4u);
        if(pci_read32(dev->bus,dev->slot,dev->func,4)&4) continue;
        if(ahci_wait_clear(h+GHC,1,1000)<0) continue;
        AHCI_WR(h+GHC,(AHCI_RD(h+GHC)|0x80000000u)&~2u);
        if(!(AHCI_RD(h+GHC)&0x80000000u)) continue;
        u32 implemented=AHCI_RD(h+PI),np=(AHCI_RD(h)&31)+1;
        int quiescent=1;
        for(u32 port=0;port<np;port++) if(implemented&(1u<<port)) {
            volatile u32 *r=h+0x40+port*32;
            AHCI_WR(r+IE,0);
            if(ahci_stop(r)<0) { quiescent=0;break; }
        }
        if(!quiescent) continue; /* BM stays disabled; no old DMA revived */
        pci_write32(dev->bus,dev->slot,dev->func,4,pci_cmd|6);
        if((pci_read32(dev->bus,dev->slot,dev->func,4)&6)!=6) continue;
        for(u32 port=0;port<np && ahci_port_count<AHCI_PORTS;port++) {
            if(!(implemented&(1u<<port))) continue;
            volatile u32 *r=h+0x40+port*32;
            if((AHCI_RD(r+SSTS)&0xF0F)!=0x103 || AHCI_RD(r+SIG)!=0x101) continue;
            ahci_port_t *p=&ahci_ports[ahci_port_count++];p->regs=r;p->number=port;
            if(ahci_stop(p->regs)<0 || AHCI_RD(r+CI) || AHCI_RD(r+SACT)) { p->fenced=1;continue; }
            u32 list,fis;
            if(AHCI_DMA(p->headers,sizeof(p->headers),&list)<0 || (list&1023) ||
               AHCI_DMA(p->received,sizeof(p->received),&fis)<0 || (fis&255)) { p->fenced=1;continue; }
            AHCI_WR(r+IE,0);AHCI_WR(r+IS,0xFFFFFFFFu);AHCI_WR(r+SERR,0xFFFFFFFFu);
            AHCI_WR(r+CLB,list);AHCI_WR(r+CLB+1,0);
            AHCI_WR(r+FB,fis);AHCI_WR(r+FB+1,0);
            AHCI_WR(r+CMD,AHCI_RD(r+CMD)|FRE);
            AHCI_WR(r+CMD,AHCI_RD(r+CMD)|ST);
            if(ahci_identify(p)==0) ahci_drive_port[ahci_drive_count++]=(int)(p-ahci_ports);
        }
    }
}
int ahci_count(void) { return ahci_drive_count; }
static ahci_port_t *ahci_drive(int i) { return i>=0 && i<ahci_drive_count?&ahci_ports[ahci_drive_port[i]]:NULL; }
u32 ahci_sectors(int i) { ahci_port_t *p=ahci_drive(i);return p && !p->fenced?p->sectors:0; }
const char *ahci_model(int i) { ahci_port_t *p=ahci_drive(i);return p?p->model:"no SATA disk"; }
const char *ahci_status(int i) { ahci_port_t *p=ahci_drive(i);return !p?"absent":p->fenced?"fenced; reboot required":p->flush_ext?"DMA, default RO; WAL writes gated":"DMA, read-only (no FLUSH EXT)"; }
u32 ahci_generation(int i) {
    ahci_port_t *p=ahci_drive(i);
    if(!p || p->fenced) return 0;
    if((AHCI_RD(p->regs+SSTS)&15)!=3 || (AHCI_RD(p->regs+IS)&IO_ERRORS)) {
        ahci_fail(p);return 0;
    }
    return p->generation;
}
void ahci_fat_revoke(int i) { ahci_port_t *p=ahci_drive(i);if(p) p->write_allowed=0; }
void ahci_rescan(void) {
    for(int i=0;i<ahci_drive_count;i++) {
        ahci_fat_revoke(i);
        if(ahci_generation(i)) {
            if(!++ahci_epoch) ++ahci_epoch;
            ahci_drive(i)->generation=ahci_epoch;
        }
    }
}
int ahci_fat_authorize(int i,u32 start,u32 count,u32 generation) {
    ahci_fat_revoke(i);ahci_port_t *p=ahci_drive(i);
    if(!p || p->busy || !p->flush_ext || !generation || generation!=ahci_generation(i) ||
       !count || start>=p->sectors || count>p->sectors-start) return -1;
    p->write_start=start;p->write_count=count;p->write_allowed=1;return 0;
}
static int ahci_transfer(int i,u32 lba,u8 count,void *buf,int write) {
    ahci_port_t *p=ahci_drive(i);
    if(!p || !ahci_generation(i) || !buf || !count || lba>=p->sectors || count>p->sectors-lba) return -1;
    if(write && (!p->write_allowed || lba<p->write_start || lba-p->write_start>=p->write_count ||
       count>p->write_count-(lba-p->write_start))) return -1;
    if(__sync_lock_test_and_set(&p->busy,1)) return -1;
    int result=0;
    while(count) {
        u8 n=count>64?64:count;
        if(write) memcpy(p->data,buf,(u32)n*512);
        if(ahci_command(p,write?0x35:0x25,lba,n)<0) { result=-1;break; }
        if(!write) memcpy(buf,p->data,(u32)n*512);
        buf=(u8 *)buf+(u32)n*512;lba+=n;count-=n;
    }
    __sync_lock_release(&p->busy);return result;
}
int ahci_read(int i,u32 lba,u8 count,void *buf) { return ahci_transfer(i,lba,count,buf,0); }
int ahci_write(int i,u32 lba,u8 count,const void *buf) { return ahci_transfer(i,lba,count,(void *)buf,1); }
int ahci_sync(int i) {
    ahci_port_t *p=ahci_drive(i);
    if(!p || !ahci_generation(i) || !p->write_allowed || !p->flush_ext) return -1;
    if(__sync_lock_test_and_set(&p->busy,1)) return -1;
    int result=ahci_command(p,0xEA,0,0);
    __sync_lock_release(&p->busy);return result;
}
