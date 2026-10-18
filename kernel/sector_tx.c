/* Undo WAL: backup+barrier, indexed header+barrier, THEN metadata write.
 * New file bodies must be copy-on-write and are not journalled here.
 * Closing the log is irreversible: on an ambiguous close, never roll back
 * targets while a clean header might already be durable. Fence and recover. */
#include "sector_tx.h"
#if KV_STORAGE_LAB
volatile uint32_t stx_lab_step,stx_lab_pause_at,stx_lab_unplug;
volatile uint32_t stx_lab_events[256][2];
static void lab_step(sector_tx_t *t,uint32_t phase,uint32_t lba) {
    unsigned n=stx_lab_step;
    if(n<256) { stx_lab_events[n][0]=phase;stx_lab_events[n][1]=lba; }
    stx_lab_step=n+1;
    while(stx_lab_pause_at && stx_lab_pause_at==stx_lab_step) {
        /* Detach tests release the pause only after an actual transport error.
         * No WRITE is retried; these extra flushes are diagnostic-only. */
        if(stx_lab_unplug && t->io.sync(t->io.ctx)<0) break;
        __asm__ volatile("pause");
    }
}
#else
#define lab_step(t,phase,lba) ((void)0)
#endif
static int write_sector(sector_tx_t *t,uint32_t lba,const void *b) {
    lab_step(t,1,lba);int r=t->io.write(t->io.ctx,lba,b);lab_step(t,2,lba);return r;
}
static int barrier(sector_tx_t *t) {
    lab_step(t,3,~0u);int r=t->io.sync(t->io.ctx);lab_step(t,4,~0u);return r;
}
#if STX_DURABLE
uint32_t stx_crc32(const void *data,unsigned n) {
    const unsigned char *p=data;uint32_t c=~0u;
    while(n--) { c^=*p++;for(unsigned i=0;i<8;i++) c=(c>>1)^((0u-(c&1))&0xedb88320u); }
    return ~c;
}
static void put(unsigned char *p,uint32_t n) { for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(n>>(8*i)); }
static uint32_t get(const unsigned char *p) { return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
#endif
static int config_ok(const stx_io_t *io) {
    if(!io || !io->read || !io->write || !io->sync || !io->count || io->count>~0u-io->first) return 0;
    if(io->journal_count && !STX_DURABLE) return 0;
    if(!io->journal_count) return 1; /* RAM undo only; not power-loss protection. */
    return io->journal_count>=STX_MAX+1 && io->journal_count<=~0u-io->journal &&
        (io->journal>=io->first+io->count || io->first>=io->journal+io->journal_count);
}
static int target_ok(const stx_io_t *io,uint32_t lba) { return lba>=io->first && lba-io->first<io->count; }
#if STX_DURABLE
static int header(sector_tx_t *t,int active) {
    unsigned char b[512]={0};
    const char magic[8]="KVSTX01";for(unsigned i=0;i<8;i++) b[i]=(unsigned char)magic[i];
    put(b+8,1);put(b+12,t->io.identity);put(b+16,t->io.first);put(b+20,t->io.count);
    put(b+24,t->io.journal);put(b+28,t->io.journal_count);put(b+32,(uint32_t)active);put(b+36,active?t->n:0);
    if(active) for(unsigned i=0;i<t->n;i++) { put(b+40+8*i,t->lba[i]);put(b+44+8*i,t->crc[i]); }
    put(b+508,stx_crc32(b,508));
    return write_sector(t,t->io.journal,b)<0 || barrier(t)<0?-1:0;
}
int stx_format(sector_tx_t *t,const stx_io_t *io) {
    if(!config_ok(io) || !io->journal_count) return -1;
    unsigned char b[512];
    for(unsigned i=0;i<io->journal_count;i++) {
        if(io->read(io->ctx,io->journal+i,b)<0) return -1;
        for(unsigned j=0;j<512;j++) if(b[j]) return -1;
    }
    t->io=*io;t->n=0;t->active=t->failed=t->closing=0;
    return header(t,0);
}
#else
int stx_format(sector_tx_t *t,const stx_io_t *io) { (void)t;(void)io;return -1; }
#endif
int stx_load(sector_tx_t *t,const stx_io_t *io) {
    if(!config_ok(io)) return -1;
    t->io=*io;t->n=0;t->active=t->failed=t->closing=0;
    if(!io->journal_count) return 0;
    #if STX_DURABLE
    unsigned char b[512];
    if(io->read(io->ctx,io->journal,b)<0) return -1;
    const char magic[8]="KVSTX01";
    for(unsigned i=0;i<8;i++) if(b[i]!=(unsigned char)magic[i]) return -1;
    if(get(b+8)!=1 || get(b+12)!=io->identity || get(b+16)!=io->first || get(b+20)!=io->count ||
       get(b+24)!=io->journal || get(b+28)!=io->journal_count || get(b+32)>1 ||
       get(b+36)>STX_MAX || get(b+508)!=stx_crc32(b,508)) return -1;
    unsigned n=get(b+36);
    if(!get(b+32)) return n?-1:0;
    /* Validate ALL addresses and backup CRCs before restoring any sector. */
    for(unsigned i=0;i<n;i++) {
        t->lba[i]=get(b+40+8*i);t->crc[i]=get(b+44+8*i);
        if(!target_ok(io,t->lba[i])) return -1;
        for(unsigned j=0;j<i;j++) if(t->lba[j]==t->lba[i]) return -1;
        if(io->read(io->ctx,io->journal+1+i,t->old[i])<0 || stx_crc32(t->old[i],512)!=t->crc[i]) return -1;
    }
    t->n=n;t->active=1;
    return 0;
    #else
    return 0;
    #endif
}
int stx_recover(sector_tx_t *t,const stx_io_t *io) {
    if(stx_load(t,io)<0) return -1;
    return t->active?stx_abort(t):0;
}
int stx_begin(sector_tx_t *t,const stx_io_t *io) {
    if(stx_recover(t,io)<0) return -1;
    t->active=1;
#if STX_DURABLE
    if(io->journal_count && header(t,1)<0) { t->failed=1;return -1; }
#endif
    return 0;
}
int stx_write(sector_tx_t *t,uint32_t lba,const void *data) {
    if(!t->active || t->failed || t->closing) return -1;
    if(!data || !target_ok(&t->io,lba)) { t->failed=1;return -1; }
    unsigned i=0;while(i<t->n && t->lba[i]!=lba) i++;
    if(i==t->n) {
        if(i==STX_MAX || t->io.read(t->io.ctx,lba,t->old[i])<0) { t->failed=1;return -1; }
        t->lba[i]=lba;t->n++;
#if STX_DURABLE
        t->crc[i]=stx_crc32(t->old[i],512);
        if(t->io.journal_count &&
           (write_sector(t,t->io.journal+1+i,t->old[i])<0 || barrier(t)<0 || header(t,1)<0)) {
            t->failed=1;return -1;
        }
#endif
    }
    if(write_sector(t,lba,data)<0) { t->failed=1;return -1; }
    return 0;
}
int stx_commit(sector_tx_t *t) {
    if(!t->active || t->failed) return -1;
    if(barrier(t)<0) { t->failed=1;return -1; }
    #if STX_DURABLE
    if(t->io.journal_count) {
        t->closing=1;
        if(header(t,0)<0) { t->failed=1;return -1; }
    }
    #endif
    t->active=0;t->n=0;return 0;
}
int stx_abort(sector_tx_t *t) {
    if(!t->active || t->closing) return -1;
    for(unsigned i=t->n;i;i--) if(write_sector(t,t->lba[i-1],t->old[i-1])<0) return -1;
    if(barrier(t)<0) return -1;
#if STX_DURABLE
    if(t->io.journal_count) { t->closing=1;if(header(t,0)<0) return -1; }
#endif
    t->active=0;t->n=0;return 0;
}
