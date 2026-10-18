/* Private FAT implementation, included once after the directory/transaction helpers.
 * A host filesystem tool preallocates KVANTWAL.SYS; we never choose free sectors,
 * allocate this file without a WAL, format a volume, or edit a partition table.
 * The owner sector is immutable. All addresses in the log are volume-relative.
 */
#define WAL_DIR_LIMIT 1024u
static u8 wal_buf[512],wal_fat[4096],wal_first_fat[4096];
static u32 wal_dirs[WAL_DIR_LIMIT],wal_ndirs,wal_current,wal_entries,wal_seen;

/* Locate only: do NOT require a consistent complete root chain before undo.
 * Root expansion can have been interrupted AFTER the existing owner entry.
 * The unmodified prefix up to that entry is sufficient to locate the log;
 * the entire old namespace is subsequently checked through the undo overlay. */
static int wal_find(fat_vol_t *v) {
    u32 c=v->root_cluster;
    for(u32 hops=0;hops<100000;hops++) {
        if(c<2 || c-2>=v->total_clusters) return -1;
        u32 sec=cluster_to_sector(v,c);
        for(u32 j=0;j<v->sectors_per_cluster;j++) {
            if(!fat_read_sectors(v,sec+j,1,wal_buf)) return -1;
            for(u32 off=0;off<512;off+=32) {
                fat_dirent_t *de=(fat_dirent_t *)(wal_buf+off);
                if(!de->name[0]) return 0;
                if(de->name[0]==0xE5 || de->attr==FAT_ATTR_LFN) continue;
                if(memcmp(de->name,"KVANTWALSYS",11)) continue;
                if((de->attr&0xDF)!=7) return -1; /* RO, hidden, system; archive optional */
                v->wal_cluster=entry_cluster(v,de);
                v->wal_clusters=(STX_MAX+2+v->sectors_per_cluster-1)/v->sectors_per_cluster;
                v->wal_sectors=v->wal_clusters*v->sectors_per_cluster;
                if(de->size!=v->wal_sectors*512 || v->wal_cluster<2 ||
                   v->wal_cluster-2>=v->total_clusters || v->wal_clusters>v->total_clusters-(v->wal_cluster-2)) return -1;
                /* BPB root is a reference too; it has no directory entry callback. */
                if(v->root_cluster>=v->wal_cluster && v->root_cluster-v->wal_cluster<v->wal_clusters) return -1;
                v->wal_sector=cluster_to_sector(v,v->wal_cluster);
                v->wal_dir=sec+j;v->wal_off=off;memcpy(v->wal_entry,de,32);
                return 1;
            }
        }
        c=fat_get(v,c);if(v->error) return -1;
        if(fat_is_eoc(v,c)) return 0;
    }
    return -1;
}
static int wal_fat_entry(fat_vol_t *v,u32 i,u32 n) {
    if(i<2 || i-2>=v->total_clusters) return 1;
    if(i>=v->wal_cluster && i-v->wal_cluster<v->wal_clusters) {
        if(i-v->wal_cluster+1==v->wal_clusters) { if(n<FAT_EOC32) return 0; }
        else if(n!=i+1) return 0;
    }
    /* No other allocated OR lost chain may enter any part of the WAL file. */
    if(n>=v->wal_cluster && n-v->wal_cluster<v->wal_clusters &&
       !(i>=v->wal_cluster && i-v->wal_cluster+1<v->wal_clusters && n==i+1)) return 0;
    return 1;
}
static int wal_check_fats(fat_vol_t *v) {
    for(u32 s=0;s<v->fat_size;s+=8) {
      u32 count=v->fat_size-s;if(count>8) count=8;
      for(u32 f=0;f<v->num_fats;f++) {
        u32 sec=v->fat_start+f*v->fat_size+s;
        if(!live(v) || blk_read(v->disk,v->lba+sec,(u8)count,wal_fat)<0) return 0;
        /* Validate physical links too: recovery must not overwrite data now
         * owned by another chain. Mirror divergence is allowed only where a
         * fully CRC-validated undo record explains the interrupted write. */
        for(u32 off=0;off<count*512;off+=4) {
            u32 n;memcpy(&n,wal_fat+off,4);
            if(!wal_fat_entry(v,s*128+off/4,n&0x0FFFFFFFu)) return 0;
        }
        for(unsigned j=0;j<fat_tx.n;j++) if(fat_tx.lba[j]>=sec && fat_tx.lba[j]-sec<count)
            memcpy(wal_fat+(fat_tx.lba[j]-sec)*512,fat_tx.old[j],512);
        for(u32 off=0;off<count*512;off+=4) {
            u32 n,first;memcpy(&n,wal_fat+off,4);n&=0x0FFFFFFFu;
            if(!wal_fat_entry(v,s*128+off/4,n)) return 0;
            if(s==0 && off==4 && n!=0x0FFFFFFFu) return 0; /* dirty/error BEFORE our tx: external fsck required */
            if(f) { memcpy(&first,wal_first_fat+off,4);if(n!=(first&0x0FFFFFFFu)) return 0; }
        }
        if(!f) memcpy(wal_first_fat,wal_fat,count*512);
      }
    }
    return 1;
}
static int wal_dir_cb(void *ctx,const char *name,fat_dirent_t *de,u32 sec,u32 off) {
    fat_vol_t *v=ctx;u32 c=entry_cluster(v,de);
    int owner=sec==v->wal_dir && off==v->wal_off;
    if(++wal_entries>262144 || (owner && (wal_current!=v->root_cluster || memcmp(de,v->wal_entry,32))) ||
       (!owner && c>=v->wal_cluster && c-v->wal_cluster<v->wal_clusters) ||
       (!owner && wal_current==v->root_cluster && !memcmp(de->name,"KVANTWALSYS",11))) goto bad;
    if(owner) { wal_seen++;return 1; }
    if(!memcmp(de->name,".          ",11) || !memcmp(de->name,"..         ",11)) return 1;
    (void)name;
    if(de->attr&FAT_ATTR_DIR) {
        if(c<2 || c-2>=v->total_clusters || wal_ndirs>=WAL_DIR_LIMIT) goto bad;
        for(u32 i=0;i<wal_ndirs;i++) if(wal_dirs[i]==c) goto bad;
        wal_dirs[wal_ndirs++]=c;
    }
    return 1;
bad:
    v->error=1;return 0;
}
static int wal_check_namespace(fat_vol_t *v) {
    wal_ndirs=1;wal_dirs[0]=v->root_cluster;wal_seen=wal_entries=0;
    for(u32 i=0;i<wal_ndirs;i++) {
        wal_current=wal_dirs[i];
        if(!walk_dir(v,wal_current,wal_dir_cb,v) || v->error) return 0;
    }
    return wal_seen==1;
}
static int wal_prepare(fat_vol_t *v,const u8 *bpb) {
    v->overlay=v->file_wal=0;
    if(!v->is_fat32 || v->bytes_per_sector!=512) return 0;
    int found=wal_find(v);if(found<=0) return found;
    if(!v->mirrored || v->num_fats!=2 || v->extent>=FAT_WAL_VIRTUAL) return -1;
    if(!fat_read_sectors(v,v->wal_sector,1,wal_buf)) return -1;
    const char magic[16]="KVANTFATWAL1";
    u32 fields[16],crc;memcpy(fields,wal_buf+64,sizeof(fields));memcpy(&crc,wal_buf+508,4);
    const u32 expected[16]={1,stx_crc32(bpb,512),v->serial,v->extent,v->sectors_per_cluster,
        v->wal_cluster,v->wal_clusters,v->wal_sectors,v->wal_dir,v->wal_off,FAT_WAL_VIRTUAL,
        STX_MAX+1,v->root_cluster,v->first_data_sector,v->num_fats,v->fat_size};
    if(memcmp(wal_buf,magic,16) || crc!=stx_crc32(wal_buf,508) || memcmp(fields,expected,sizeof(fields))) return -1;
    v->file_wal=1;v->journal=FAT_WAL_VIRTUAL;v->journal_count=STX_MAX+1;
    stx_io_t io=tx_io(v);
    if(stx_load(&fat_tx,&io)<0) return -1;
    for(unsigned i=0;i<fat_tx.n;i++) {
        if(!wal_target(v,fat_tx.lba[i]) || (fat_tx.lba[i]==v->wal_dir &&
           memcmp(fat_tx.old[i]+v->wal_off,v->wal_entry,32))) return -1;
    }
    /* Prove exclusive file ownership against the ORIGINAL metadata, before
     * granting ANY write, not against a partially published current tree. */
    v->overlay=1;
    int valid=wal_check_fats(v) && wal_check_namespace(v);
    v->overlay=0;
    if(!valid || !live(v)) return -1;
    if(fat_tx.active) {
        if(!wal_lease(v)) return -1;
        int r=stx_abort(&fat_tx);wal_release(v);
        if(r<0 || !live(v)) return -1;
        kputs("FAT owned WAL recovered BEFORE namespace publication\n");
    }
    return 1;
}
int fat_write_mode(const char *name,int enable) {
    vfs_volume_t *m=vfs_find(name);
    if(!m || m->kind!=FS_FAT32) return -1;
    fat_vol_t *v=m->fs;
    if(!io_enter(v) || v->writing) return -1;
    m->writable=0;
    if(!enable) return 0;
    if(!v->file_wal) return -1;
    if(!fat_read_sectors(v,0,1,buf_io) || wal_prepare(v,buf_io)!=1) {
        /* Never leave a formerly enrolled volume on the RAM-undo fallback. */
        fence(v);return -1;
    }
    if(!wal_lease(v)) return -1;
    wal_release(v);m->writable=1;return 0;
}
