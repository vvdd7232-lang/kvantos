/* Operation-local FAT/FSInfo sector working set. Never caches file/directory
 * data or survives an API call. Dirty eviction uses the existing undo WAL:
 * backup + barriers BEFORE the first physical target write, unchanged format.
 * No flush after an operation fails: discard RAM first, then undo physical I/O. */
#define FAT_META_SLOTS 8
static struct fat_meta_entry {
    u8 data[SECTOR_MAX];
    u32 sector;
    u8 valid,dirty;
} fat_meta[FAT_META_SLOTS];
static fat_vol_t *fat_meta_owner;
static unsigned fat_meta_next;
static void fat_meta_begin(fat_vol_t *v) {
    fat_meta_owner=v;fat_meta_next=0;
    for(unsigned i=0;i<FAT_META_SLOTS;i++) fat_meta[i].valid=fat_meta[i].dirty=0;
}
static int fat_meta_eligible(fat_vol_t *v,u32 sec) {
    return fat_meta_owner==v && !v->overlay &&
        ((sec>=v->fat_start && (sec-v->fat_start)/v->fat_size<v->num_fats) ||
         (v->fsinfo_sector && sec==v->fsinfo_sector) || (v->backup_fsinfo && sec==v->backup_fsinfo));
}
static int fat_meta_flush_one(fat_vol_t *v,struct fat_meta_entry *e) {
    if(!e->dirty) return 1;
    if(!v->writing || v->error || !live(v)) { v->error=1;return 0; }
    u32 factor=v->bytes_per_sector/512;
    for(u32 i=0;i<factor;i++)
        if(stx_write(&fat_tx,(v->file_wal?0:v->lba)+e->sector*factor+i,e->data+512*i)<0) {
            v->error=1;return 0;
        }
    e->dirty=0;return 1;
}
static int fat_meta_flush(fat_vol_t *v) {
    if(fat_meta_owner!=v) return 1;
    for(unsigned i=0;i<FAT_META_SLOTS;i++) if(!fat_meta_flush_one(v,&fat_meta[i])) return 0;
    return 1;
}
static struct fat_meta_entry *fat_meta_fetch(fat_vol_t *v,u32 sec,int load) {
    for(unsigned i=0;i<FAT_META_SLOTS;i++)
        if(fat_meta[i].valid && fat_meta[i].sector==sec) return &fat_meta[i];
    unsigned i=0;while(i<FAT_META_SLOTS && fat_meta[i].valid) i++;
    if(i==FAT_META_SLOTS) i=fat_meta_next++%FAT_META_SLOTS;
    struct fat_meta_entry *e=&fat_meta[i];
    if(!fat_meta_flush_one(v,e)) return 0;
    e->valid=0;
    if(load && blk_read(v->disk,v->lba+sec*(v->bytes_per_sector/512),v->bytes_per_sector/512,e->data)<0) {
        v->error=1;return 0;
    }
    e->sector=sec;e->valid=1;return e;
}
