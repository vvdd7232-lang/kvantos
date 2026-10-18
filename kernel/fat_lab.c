/* Actual VFS file operations on explicitly enrolled disposable USB images.
 * This diagnostic, its buffers and its pause controls do not exist normally. */
#include "kernel.h"
#include "vfs.h"
#include "sector_tx.h"
#if KV_STORAGE_LAB
static volatile int fat_lab_stage,fat_lab_result;
static u8 body[65535],readback[65535];
static void done(int result,int rw) {
    fat_lab_result=result;
    kprintf("M2FAT result=%d rw=%d steps=%u\n",result,rw,(unsigned)stx_lab_step);
    fat_lab_stage=3;
}
void fat_lab(const char *mode,int cut) {
    fat_lab_stage=1;fat_lab_result=-999;
    stx_lab_step=stx_lab_pause_at=stx_lab_unplug=0;
    vfs_volume_t *v=NULL;
    for(int i=0;i<vfs_volume_count();i++) {
        vfs_volume_t *m=vfs_volume(i);
        if(m->disk==4 && m->kind==FS_FAT32 && m->writable) { v=m;break; }
    }
    if(!v) { done(-2,0);return; }
    char path[VFS_MAX_PATH],other[VFS_MAX_PATH];
    ksnprintf(path,sizeof(path),"/mnt/%s/Old long name.txt",v->name);
    for(unsigned i=0;i<sizeof(body);i++) body[i]=(u8)(i*19+7);
    if(cut<-256 || cut>256) { done(-14,v->writable);return; }
    stx_lab_unplug=cut<0;stx_lab_pause_at=(u32)(cut<0?-cut:cut);
    fat_lab_stage=2;int r=-14;
    if(!strcmp(mode,"arm")) { done(0,v->writable);return; } /* next real GUI/VFS operation */
    if(!strcmp(mode,"overwrite")) r=vfs_write(path,body,1300);
    else if(!strcmp(mode,"empty")) r=vfs_write(path,NULL,0);
    else if(!strcmp(mode,"large")) {
        ksnprintf(other,sizeof(other),"/mnt/%s/Large.txt",v->name);
        r=vfs_write(other,body,sizeof(body));
        if(r==(int)sizeof(body) && (vfs_read(other,0,readback,sizeof(readback))!=r || memcmp(body,readback,sizeof(body)))) r=-20;
    } else if(!strcmp(mode,"create")) {
        ksnprintf(other,sizeof(other),"/mnt/%s/Новый длинный файл.txt",v->name);
        r=vfs_write(other,body,1300);
    } else if(!strcmp(mode,"mkdir")) {
        ksnprintf(other,sizeof(other),"/mnt/%s/New long directory",v->name);r=vfs_mkdir(other);
    } else if(!strcmp(mode,"remove")) r=vfs_remove(path);
    else if(!strcmp(mode,"rmdir")) {
        ksnprintf(other,sizeof(other),"/mnt/%s/Empty long directory",v->name);r=vfs_remove(other);
    } else if(!strcmp(mode,"verify")) {
        r=vfs_read(path,0,readback,sizeof(readback));
        if(r==1000) { for(unsigned i=0;i<1000;i++) if(readback[i]!='O') { r=-20;break; } }
        else if(r==1300) { if(memcmp(readback,body,1300)) r=-20; }
        else if(r!=0) r=-20;
    } else if(!strcmp(mode,"stale")) {
        usb_rescan(); /* Deliberately leave the OLD VFS mount alive. */
        /* Fresh validated enrollment establishes a NEW live grant; merely
           clearing all grants would not test generation fencing. */
        if(fat_mount("lab-new",4,v->lba_start,v->lba_count)<0 ||
           !vfs_writable("/mnt/lab-new/") || blk_sync(4)<0) { done(-21,0);return; }
        r=vfs_write(path,body,1300);
        if(r>=0 || stx_lab_step || vfs_read(path,0,readback,sizeof(readback))>=0 || v->writable) r=-21;
    } else if(!strcmp(mode,"guard")) {
        u32 cap=blk_sectors(4),end=v->lba_start+v->lba_count,journal=cap-1023;
        r=0;
        if(blk_write(4,0,1,body)>=0 || blk_write(4,end,1,body)>=0 ||
           blk_write(4,end-1,2,body)>=0 || blk_write(4,journal-1,1,body)>=0 ||
           blk_write(4,journal+49,1,body)>=0 || blk_write(4,cap-33,1,body)>=0) r=-22;
    }
    stx_lab_pause_at=stx_lab_unplug=0;
    done(r,v->writable);
}
#endif
