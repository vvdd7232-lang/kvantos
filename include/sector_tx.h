#ifndef KV_SECTOR_TX_H
#define KV_SECTOR_TX_H
#include <stdint.h>
#define STX_MAX 48
/* Durable protocol shared by owned FAT journal files and explicit lab fixtures.
 * With journal_count=0 this remains bounded RAM undo, NOT crash protection. */
#define STX_DURABLE 1
uint32_t stx_crc32(const void *,unsigned);
#if KV_STORAGE_LAB
extern volatile uint32_t stx_lab_step,stx_lab_pause_at,stx_lab_unplug;
extern volatile uint32_t stx_lab_events[256][2];
#endif
/* Synchronous 512-byte I/O: 0 success, negative failure. Only one writer. */
typedef struct {
    void *ctx;
    int (*read)(void *,uint32_t,void *);
    int (*write)(void *,uint32_t,const void *);
    int (*sync)(void *);
    uint32_t first,count,journal,journal_count,identity;
} stx_io_t;
typedef struct {
    stx_io_t io;
    uint32_t lba[STX_MAX],crc[STX_MAX];
    unsigned char old[STX_MAX][512];
    unsigned n;
    int active,failed,closing;
} sector_tx_t;
/* Provisioning is explicit, only over an entirely zeroed journal extent. */
int stx_format(sector_tx_t *,const stx_io_t *);
/* Read/validate all undo records without issuing a single write or barrier. */
int stx_load(sector_tx_t *,const stx_io_t *);
int stx_recover(sector_tx_t *,const stx_io_t *);
int stx_begin(sector_tx_t *,const stx_io_t *);
int stx_write(sector_tx_t *,uint32_t,const void *);
int stx_commit(sector_tx_t *);
int stx_abort(sector_tx_t *);
#endif
