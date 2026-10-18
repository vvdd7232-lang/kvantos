/* Actual production metadata working set; durable ordering is separately
 * exercised by sector_tx and the real FAT owned-WAL crash-cut suite. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define SECTOR_MAX 4096
typedef struct {
    u32 fat_start,fat_size,num_fats,fsinfo_sector,backup_fsinfo;
    u32 bytes_per_sector,lba,disk;
    int overlay,writing,error,file_wal,alive;
} fat_vol_t;
static u8 media[256][512];
static int reads,writes,fail_read,fail_write,fat_tx;
static int live(fat_vol_t *v) { return v->alive; }
static int blk_read(u32 disk,u32 lba,unsigned n,void *buf) {
    assert(!disk && lba+n<=256);reads++;
    if(fail_read) return -1;
    memcpy(buf,media[lba],n*512);return 0;
}
static int stx_write(int *tx,u32 lba,const void *buf) {
    assert(tx==&fat_tx && lba<256);
    if(fail_write) return -1;
    writes++;memcpy(media[lba],buf,512);return 0;
}
#include "../../kernel/fat_meta.h"
int main(void) {
    for(unsigned factor=1;factor<=8;factor*=2) {
        fat_vol_t v={.fat_start=8,.fat_size=8,.num_fats=2,.fsinfo_sector=1,.backup_fsinfo=7,
                     .bytes_per_sector=512*factor,.lba=3,.alive=1};
        memset(media,0xA5,sizeof(media));reads=writes=0;fat_meta_begin(&v);
        assert(fat_meta_eligible(&v,8) && fat_meta_eligible(&v,23));
        assert(fat_meta_eligible(&v,1) && fat_meta_eligible(&v,7));
        assert(!fat_meta_eligible(&v,0) && !fat_meta_eligible(&v,24));
        v.overlay=1;assert(!fat_meta_eligible(&v,8));v.overlay=0;
        struct fat_meta_entry *e=fat_meta_fetch(&v,8,1);assert(e && reads==1);
        assert(fat_meta_fetch(&v,8,1)==e && reads==1);
        memset(e->data,0x19,v.bytes_per_sector);e->dirty=1;v.writing=1;
        /* Fill and evict the dirty first slot; old flush API remains in charge. */
        for(unsigned s=9;s<17;s++) assert(fat_meta_fetch(&v,s,1));
        assert(writes==(int)factor && media[3+8*factor][0]==0x19);
        assert(fat_meta_flush(&v) && writes==(int)factor);
        /* Discard must never flush an aborted operation. */
        e=fat_meta_fetch(&v,8,0);memset(e->data,0x37,v.bytes_per_sector);e->dirty=1;
        fat_meta_begin(0);assert(!fat_meta_eligible(&v,8) && fat_meta_flush(&v));
        assert(writes==(int)factor);fat_meta_begin(&v);
        e=fat_meta_fetch(&v,8,1);assert(e->data[0]==0x19);
        e->dirty=1;fail_write=1;assert(!fat_meta_flush(&v) && v.error && e->dirty);
        fail_write=0;v.error=0;fat_meta_begin(&v);
        fail_read=1;assert(!fat_meta_fetch(&v,8,1) && v.error);
        fail_read=0;v.error=0;fat_meta_begin(&v);
        e=fat_meta_fetch(&v,8,1);e->dirty=1;v.alive=0;
        assert(!fat_meta_flush(&v) && v.error);fat_meta_begin(0);
    }
    puts("PASS FAT metadata: scope, overlay exclusion, dirty eviction, discard, read/write EIO, live fence; 512/1024/2048/4096 bytes");
}
