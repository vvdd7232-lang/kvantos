/* Stable bus IDs and independently enforced AHCI read-only policy. */
#include <assert.h>
#include <stdio.h>
#include "kernel.h"
static volatile int a,u,h;
static int last=-1,writes,syncs,ahci_grant,usb_grant;
int ata_count(void) { return a; }
int usb_count(void) { return u; }
int ahci_count(void) { return h; }
u32 ata_sectors(int i) { return i>=0 && i<a?100:0; }
u32 usb_sectors(int i) { return i>=0 && i<u?200:0; }
u32 ahci_sectors(int i) { return i>=0 && i<h?300:0; }
int ata_read(int i,u32 l,u8 n,void *b) { (void)l;(void)n;(void)b;last=i;return 0; }
int usb_read(int i,u32 l,u8 n,void *b) { return ata_read(4+i,l,n,b); }
int ahci_read(int i,u32 l,u8 n,void *b) { return ata_read(12+i,l,n,b); }
int ata_write(int i,u32 l,u8 n,const void *b) { (void)i;(void)l;(void)n;(void)b;writes++;return 0; }
int usb_write(int i,u32 l,u8 n,const void *b) { return ata_write(i,l,n,b); }
int ata_sync(int i) { (void)i;syncs++;return 0; }
int usb_sync(int i) { return ata_sync(i); }
u32 ahci_generation(int i) { return ahci_sectors(i)?91:0; }
u32 usb_generation(int i) { return usb_sectors(i)?92:0; }
int ahci_fat_authorize(int i,u32 s,u32 n,u32 g) { ahci_grant=i==0 && s==1 && n==2 && g==91;return ahci_grant?0:-1; }
void ahci_fat_revoke(int i) { (void)i;ahci_grant=0; }
int usb_fat_authorize(int i,u32 s,u32 n,u32 g) { usb_grant=i==0 && s==1 && n==2 && g==92;return usb_grant?0:-1; }
void usb_fat_revoke(int i) { (void)i;usb_grant=0; }
int ahci_write(int i,u32 l,u8 n,const void *b) { return ahci_grant?ata_write(i,l,n,b):-1; }
int ahci_sync(int i) { return ahci_grant?ata_sync(i):-1; }
#include "../../kernel/blk.c"
int main(void) {
    char data[512];
    assert(!blk_count());h=1;assert(blk_count()==13 && !blk_sectors(0) && !blk_sectors(4) && blk_sectors(12)==300);
    u=8;a=4;h=8;assert(blk_count()==20);
    for(int i=0;i<20;i++) {
        assert(blk_read(i,0,1,data)==0 && last==i);
        assert(blk_is_usb(i)==(i>=4 && i<12));assert(blk_is_ahci(i)==(i>=12));
        if(i<12) assert(blk_write(i,0,1,data)==0 && blk_sync(i)==0);
        else assert(blk_write(i,0,1,data)<0 && blk_sync(i)<0);
    }
    assert(writes==12 && syncs==12);
    for(int i=-2;i<23;i++) {
        u32 n=blk_sectors(i);
        assert(blk_read(i,n,1,data)<0 && blk_write(i,n,1,data)<0);
        assert(blk_read(i,0xFFFFFFFFu,255,data)<0 && blk_read(i,0,0,data)<0 && blk_read(i,0,1,NULL)<0);
    }
    assert(blk_generation(0)==0 && blk_generation(4)==92 && blk_generation(12)==91);
    assert(blk_fat_authorize(12,1,2,91)==0 && ahci_grant && !usb_grant);
    assert(blk_write(12,1,1,data)==0 && blk_sync(12)==0);
    blk_fat_revoke(12);assert(!ahci_grant && blk_write(12,1,1,data)<0 && blk_sync(12)<0);
    assert(blk_fat_authorize(4,1,2,92)==0 && usb_grant && !ahci_grant);
    blk_fat_revoke(4);assert(!usb_grant);
    assert(blk_fat_authorize(0,99,2,0)<0 && blk_fat_authorize(0,1,2,0)==0);
    h=0;assert(blk_count()==12);u=0;assert(blk_count()==4);a=0;assert(!blk_count());
    puts("PASS block IDs: ATA 0..3, USB 4..11, AHCI 12..19; holes/bounds/null/zero and transport-enforced SATA grants");
}
