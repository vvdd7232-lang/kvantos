/* Never compiled into the normal image's executable diagnostic path.
 * Raw scratch I/O only; does not grant FAT/VFS writable mounts. */
#include "kernel.h"
#if KV_STORAGE_LAB
static volatile int storage_lab_stage,storage_lab_result;
#define PROBE_SECTORS 17
static u8 probe_data[PROBE_SECTORS*512],probe_read[PROBE_SECTORS*512],probe_marker[512];
static void finish(int result) {
    u8 key=0,asc=0,ascq=0;usb_sense(0,&key,&asc,&ascq);
    storage_lab_result=result;
    kprintf("M2LAB result=%d sense=%02x/%02x/%02x\n",result,key,asc,ascq);
    storage_lab_stage=3;
}
void storage_lab(const char *mode) {
    storage_lab_stage=1;storage_lab_result=-999;
    kputs("M2LAB: DISPOSABLE SCRATCH ONLY; THIS COMMAND NEVER ENABLES FAT/VFS\n");
    u32 sectors=usb_sectors(0),recorded=0,version=0;
    if(usb_count()!=1 || sectors<4096) { finish(-10);return; }
    u32 marker=sectors-1024,start=marker+1;
    if(blk_read(4,marker,1,probe_marker)<0) { finish(-11);return; }
    memcpy(&recorded,probe_marker+64,4);memcpy(&version,probe_marker+68,4);
    const char magic[]="KVANTOS-M2-DISPOSABLE-ONLY";
    if(memcmp(probe_marker,magic,sizeof(magic)) || recorded!=sectors || version!=1) { finish(-12);return; }
    for(unsigned i=0;i<sizeof(probe_data);i++) probe_data[i]=(u8)(i*29+(i>>8)+0x53);
    if(!strcmp(mode,"verify")) {
        if(blk_read(4,start,PROBE_SECTORS,probe_read)<0 || memcmp(probe_read,probe_data,sizeof(probe_data))) finish(-13);
        else finish(0);
        return;
    }
    if(strcmp(mode,"roundtrip") && strcmp(mode,"detach")) { finish(-14);return; }
    /* Default lock and stale-cache test. */
    if(blk_write(4,start,1,probe_data)>=0 || blk_sync(4)>=0 ||
       blk_read(4,start,PROBE_SECTORS,probe_read)<0 || usb_test_authorize(0,start,32)<0) { finish(-15);return; }
    if(blk_write(4,0,1,probe_data)>=0 || blk_write(4,start+32,1,probe_data)>=0 ||
       blk_write(4,start,255,probe_data)>=0 || blk_write(4,start,0,probe_data)>=0) { finish(-16);return; }
    storage_lab_stage=2;
    if(!strcmp(mode,"detach")) {
        u64 until=timer_ticks()+timer_hz()*5;
        while(timer_ticks()<until) {
            if(blk_write(4,start,PROBE_SECTORS,probe_data)<0) {
                if(blk_write(4,start,1,probe_data)>=0) { finish(-17);return; }
                finish(-1);return;
            }
        }
        usb_test_authorize(0,0,0);finish(-18);return;
    }
    if(blk_write(4,start,PROBE_SECTORS,probe_data)<0 || blk_sync(4)<0) {
        if(blk_write(4,start,1,probe_data)>=0) { finish(-17);return; }
        finish(-1);return;
    }
    if(blk_read(4,start,PROBE_SECTORS,probe_read)<0 || memcmp(probe_data,probe_read,sizeof(probe_data))) { finish(-19);return; }
    usb_test_authorize(0,0,0);
    if(blk_write(4,start,1,probe_data)>=0) { finish(-17);return; }
    finish(0);
}
#endif
