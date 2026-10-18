/* Real NTFS adapter: small valid resident index and hidden extension I/O. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/ntfs.c"
static u8 record[512],file_record[512];static int calls,fail_at,capture;static u32 seen_lba,block_total;static u8 seen_count;
int blk_read(int disk,u32 lba,u8 count,void *buf){
    assert(disk==0);calls++;seen_lba=lba;seen_count=count;block_total+=count;
    if(calls==fail_at)return -1;
    if(capture){memset(buf,0,(u32)count*512);return 0;}
    assert(count==1);if(lba==13)memcpy(buf,record,512);else if(lba==28)memcpy(buf,file_record,512);else assert(0);return 0;
}
static void put16(u32 at,u16 v){memcpy(record+at,&v,2);}
static void put32(u32 at,u32 v){memcpy(record+at,&v,4);}
static ntfs_vol_t fresh(int named,int extension){
    memset(record,0,sizeof(record));memset(file_record,0,sizeof(file_record));calls=fail_at=0;
    memcpy(record,"FILE",4);put16(4,0x30);put16(6,2);put16(0x30,0x1234);put16(510,0x1234);put16(0x14,0x38);
    memcpy(file_record,"FILE",4);u16 usa_off=0x30,usa_count=2,marker=0x1234,first_attr=0x38;
    memcpy(file_record+4,&usa_off,2);memcpy(file_record+6,&usa_count,2);memcpy(file_record+0x30,&marker,2);memcpy(file_record+510,&marker,2);memcpy(file_record+0x14,&first_attr,2);
    u32 data_type=AT_DATA,data_len=32,value_len=1;u16 value_off=24;
    memcpy(file_record+0x38,&data_type,4);memcpy(file_record+0x3C,&data_len,4);memcpy(file_record+0x48,&value_len,4);memcpy(file_record+0x4C,&value_off,2);file_record[0x50]='D';
    u32 end_type=AT_END;memcpy(file_record+0x58,&end_type,4);
    u32 attr=0x38,value=attr+24,vlen=named?136:48,alen=24+vlen;
    put32(attr,AT_INDEX_ROOT);put32(attr+4,alen);put32(attr+16,vlen);put16(attr+20,24);
    put32(value+16,16);put32(value+20,vlen-16);
    u32 entry=value+32;
    if(named){put32(entry,20);put16(entry+8,88);put16(entry+10,68);put32(entry+16+0x30,7);record[entry+16+0x40]=1;record[entry+16+0x41]=1;put16(entry+16+0x42,'x');entry+=88;}
    put16(entry+8,16);put16(entry+12,2);attr+=alen;
    if(extension){put32(attr,AT_ATTR_LIST);put32(attr+4,56);put32(attr+16,26);put16(attr+20,24);put32(attr+24,AT_INDEX_ALLOC);put16(attr+28,26);put32(attr+24+16,30);attr+=56;}
    put32(attr,AT_END);
    ntfs_vol_t v={0};v.bytes_per_sector=v.cluster_size=v.record_size=v.index_size=512;v.sectors_per_cluster=1;v.total_sectors=v.partition_sectors=1024;v.mft_run_count=1;v.mft_runs[0].len=64;v.mft_runs[0].lcn=8;return v;
}
int main(void){
    vfs_dirent_t e;ntfs_vol_t v=fresh(0,0);assert(ntfs_op_readdir(&v,"/",0,&e)==0 && calls==1);
    u8 size_record[512]={0},size_out[8]={0};u32 attr_type=AT_DATA,attr_len=64,high=1;u8 *size_attr=size_record+0x38;size_attr[8]=1;memcpy(size_attr,&attr_type,4);memcpy(size_attr+4,&attr_len,4);memcpy(size_attr+0x34,&high,4);
    assert(read_attr(&v,size_record,size_attr,0,size_out,sizeof(size_out),NULL)==-2);
    memset(size_attr+0x34,0,4);memcpy(size_attr+0x3C,&high,4);
    assert(read_attr(&v,size_record,size_attr,0,size_out,sizeof(size_out),NULL)==-2);
    v=fresh(1,0);assert(ntfs_op_readdir(&v,"/",0,&e)==1 && !strcmp(e.name,"x") && e.size==7);
    assert(ntfs_op_readdir(&v,"/",1,&e)==0);
    v=fresh(1,0);put32(0xB4,1);assert(ntfs_op_readdir(&v,"/",0,&e)==-1 && v.read_error);
    for(int fault=0;fault<10;fault++) {
        v=fresh(1,0);
        if(fault==0)put16(0x78,0);
        if(fault==1)put16(0x78,96);
        if(fault==2)put16(0x7a,69);
        if(fault==3)record[0xc0]=255;
        if(fault==4)record[0xc1]=4;
        if(fault==5){record[0xc2]=0;record[0xc3]=0xD8;}
        if(fault==6)put32(0x60,0xFFFFFFFF);
        if(fault==7)put32(0x64,0xFFFFFFF0);
        if(fault==8)put32(0x3c,0xFFFFFFFF);
        if(fault==9)put16(0x4c,0xFFFF);
        {int rr=ntfs_op_readdir(&v,"/",fault==1?1:0,&e);assert(rr==-1 && v.read_error);}
    }
    v=fresh(1,0);record[0xc0]=2;record[0xc2]=0x3D;record[0xc3]=0xD8;record[0xc4]=0;record[0xc5]=0xDE;
    put16(0x78,88);put16(0x7a,70);
    assert(ntfs_op_readdir(&v,"/",0,&e)==1 && (u8)e.name[0]==0xF0 && (u8)e.name[3]==0x80);
    v=fresh(1,0);fail_at=1;assert(ntfs_op_readdir(&v,"/",0,&e)==-1 && calls==1 && v.read_error);
    fail_at=0;assert(ntfs_op_readdir(&v,"/",0,&e)==1 && !v.read_error);
    v=fresh(1,1);fail_at=2;assert(ntfs_op_readdir(&v,"/",1,&e)==-1 && calls==2 && v.read_error);
    v=fresh(1,0);put16(0x14,504);assert(ntfs_op_readdir(&v,"/",0,&e)==-1 && v.read_error);
    u8 raw[88],meta[512];ntfs_entry_t parsed;int last;u32 seed=0xC001D00D;
    for(int i=0;i<50000;i++) {
        for(unsigned j=0;j<sizeof(raw);j++){seed=seed*1664525u+1013904223u;raw[j]=(u8)(seed>>24);}
        u32 remain=seed%89,n=decode_index_entry(raw,remain,&parsed,&last);
        assert(n==0 || (n<=remain && n>=16));if(n)assert(memchr(parsed.name,0,sizeof(parsed.name)));
    }
    u8 boot[512]={0};ntfs_vol_t parsed_boot;memcpy(boot+3,"NTFS    ",8);u16 bps=512;memcpy(boot+0x0B,&bps,2);boot[0x0D]=1;u32 sectors=10000,mft=4;memcpy(boot+0x28,&sectors,4);memcpy(boot+0x30,&mft,4);boot[0x40]=(u8)-10;boot[0x44]=(u8)-9;
    memset(&parsed_boot,0,sizeof(parsed_boot));assert(parse_boot(boot,&parsed_boot) && parsed_boot.record_size==1024 && parsed_boot.index_size==512);
    boot[0x40]=0x80;memset(&parsed_boot,0,sizeof(parsed_boot));assert(!parse_boot(boot,&parsed_boot));
    boot[0x40]=(u8)-10;boot[0x44]=0;assert(!parse_boot(boot,&parsed_boot));boot[0x44]=(u8)-9;
    u8 runs_raw[]={0x11,2,5,0};ntfs_run_t run[2];assert(parse_runlist(runs_raw,runs_raw+sizeof(runs_raw),0,run,2)==1 && run[0].lcn==5 && run[0].len==2);
    u8 run_bad_size[]={0x15,1,2,3,4,5,0};assert(!parse_runlist(run_bad_size,run_bad_size+sizeof(run_bad_size),0,run,2));
    u8 run_negative[]={0x11,1,0xFF,0};assert(!parse_runlist(run_negative,run_negative+sizeof(run_negative),0,run,2));
    u8 run_unterminated[]={0x11,1,1};assert(!parse_runlist(run_unterminated,run_unterminated+sizeof(run_unterminated),0,run,2));
    u8 run_vcn[]={0x11,1,1,0};assert(!parse_runlist(run_vcn,run_vcn+sizeof(run_vcn),0xFFFFFFFFu,run,2));
    for(int i=0;i<20000;i++) {
        for(unsigned j=0;j<sizeof(meta);j++){seed=seed*1664525u+1013904223u;meta[j]=(u8)(seed>>24);}
        int bad=0;u8 *found=find_attr_in(meta,sizeof(meta),AT_INDEX_ROOT,0,&bad);
        if(found)assert(found>=meta+24 && found<meta+sizeof(meta)-23);
        u8 *value;u32 value_len;u32 at=seed%sizeof(meta);
        if(resident_value(meta+at,meta+sizeof(meta),&value,&value_len))assert(value>=meta && value<=meta+sizeof(meta) && value_len<=(u32)(meta+sizeof(meta)-value));
        u8 *rp=meta+(seed%32);int nr=parse_runlist(rp,rp+32,seed,run,2);assert(nr>=0 && nr<=2);
    }
    u8 valid[88]={0};u16 length=88,key=68;memcpy(valid+8,&length,2);memcpy(valid+10,&key,2);valid[16+0x40]=1;valid[16+0x41]=1;valid[16+0x42]='x';
    assert(decode_index_entry(valid,sizeof(valid),&parsed,&last)==88 && !strcmp(parsed.name,"x"));
    valid[16+0x34]=1;assert(decode_index_entry(valid,sizeof(valid),&parsed,&last)==88 && parsed.size_overflow);valid[16+0x34]=0;
    assert(decode_index_entry(valid,15,&parsed,&last)==0);
    v=fresh(1,0);assert(ntfs_op_readdir(&v,"/missing",0,&e)==-1);
    assert(ntfs_op_readdir(&v,"/x",0,&e)==-1);
    v=fresh(0,0);capture=1;calls=block_total=0;v.bytes_per_sector=4096;v.total_sectors=16;v.partition_sectors=128;v.lba=100;
    u8 sector_buf[4096];assert(ntfs_read_sectors(&v,1,1,sector_buf) && calls==1 && seen_lba==108 && seen_count==8 && block_total==8);
    u8 many[72*512];calls=block_total=0;assert(ntfs_read_sectors(&v,0,9,many) && calls==2 && block_total==72 && seen_lba==164 && seen_count==8);
    calls=0;v.partition_sectors=7;assert(!ntfs_read_sectors(&v,1,1,sector_buf) && !calls && v.read_error);
    v.read_error=0;v.partition_sectors=128;assert(!ntfs_read_sectors(&v,16,1,sector_buf) && !calls && v.read_error);
    v.read_error=0;v.partition_sectors=0;v.lba=0xFFFFFFFCu;assert(!ntfs_read_sectors(&v,0,1,sector_buf) && !calls && v.read_error);
    assert(!partition_fits(&v,0xFFFFFFFCu,32));
    v.lba=100;v.total_sectors=4;v.partition_sectors=32;assert(partition_fits(&v,100,32) && !partition_fits(&v,100,31));capture=0;
    u8 fix_record[4096]={0};ntfs_vol_t fixvol={0};fixvol.bytes_per_sector=4096;
    u16 fixoff=0x30,fixcount=9,marker=0xA55A;memcpy(fix_record+4,&fixoff,2);memcpy(fix_record+6,&fixcount,2);memcpy(fix_record+fixoff,&marker,2);
    for(u32 i=1;i<fixcount;i++){u16 original=(u16)(0x1100+i);memcpy(fix_record+fixoff+i*2,&original,2);memcpy(fix_record+i*512-2,&marker,2);}
    assert(apply_fixup(&fixvol,fix_record,sizeof(fix_record)));
    for(u32 i=1;i<fixcount;i++){u16 got,expected=(u16)(0x1100+i);memcpy(&got,fix_record+i*512-2,2);assert(got==expected);}
    fixcount=2;memcpy(fix_record+6,&fixcount,2);assert(!apply_fixup(&fixvol,fix_record,sizeof(fix_record)));
    puts("PASS NTFS actual listing/sector/parser: 512/4096-byte geometry translates to 512-byte block API; 4Kn USA remains 512-byte stride; refuses partition/volume/LBA and 64-bit file-size overflow; malformed attribute/index offsets/length/key/name/namespace/UTF16 rejected, valid BMP/surrogate Unicode, 50000 bounded mutations ASan/UBSan; synthetic fuzz fixtures only");
}
