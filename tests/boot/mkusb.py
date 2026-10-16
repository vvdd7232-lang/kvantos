#!/usr/bin/env python3
"""Private FAT32 fixtures via dosfstools/mtools; never formats a host device."""
from pathlib import Path
import os, struct, subprocess, sys, uuid, zlib

def make(out):
    out=Path(out); out.mkdir(parents=True,exist_ok=True)
    raw=out/'flash-raw.img'
    with raw.open('wb') as f:f.truncate(64*1024*1024)
    subprocess.run(['/sbin/mkfs.vfat','-F','32','-n','KVANTIMPORT',str(raw)],check=True,stdout=subprocess.DEVNULL)
    def copy(name,data):
        tmp=out/'data.tmp';tmp.write_bytes(data)
        subprocess.run(['mcopy','-i',str(raw),str(tmp),'::'+name],check=True)
        tmp.unlink()
    proof=(b'int counter=0;\r\nvoid open(void){counter=42;}\r\n'
           b'void draw(void){clear(0);text(16,16,"USB import OK",-1,-1);status("USB import OK");}\r\n')
    copy('HELLO.C',proof)
    copy('MULTI.C',(b'// '+b'x'*70+b'\r\n')*80+proof)
    subprocess.run(['mmd','-i',str(raw),'::Sources','::Sources/Nested'],check=True)
    copy('Sources/A long source name.C',proof)
    copy('Sources/Nested/BOM.C',b'\xef\xbb\xbf'+proof)
    for name,data in [('OTHER.C',proof.replace(b'42',b'77')),('BINARY.C',b'int x=1;\0hidden'),('LARGE.C',b'x'*16000),
                      ('WIDE.C',b'x'*121),('LINES.C',b'\n'*128),('EMPTY.C',b''),
                      ('UTF16.C',b'\xff\xfei\0n\0t\0'),('NOTE.TXT','Привет с флешки!\r\nUTF-8 и CRLF.\r\n'.encode()),
                      ('TABS.TXT',b'one\r\n\ttwo')]:
        copy(name,data)
    # Independent partitioned device; real MBR + FAT32 at sector 2048.
    mbr=out/'flash-mbr.img'
    table=bytearray(512)
    struct.pack_into('<B3sB3sII',table,446,0,b'\0'*3,0x0c,b'\0'*3,2048,raw.stat().st_size//512)
    table[510:512]=b'\x55\xaa'
    with mbr.open('wb') as f,raw.open('rb') as src:
        f.write(table);f.seek(2048*512)
        while chunk:=src.read(1024*1024):f.write(chunk)
    return raw,mbr,proof.replace(b'\r',b'')
def make_gpt(raw, out, stride=128, count=128, fat_slot=1):
    """Windows-like GPT: MSR in slot 1, FAT32 Basic Data in slot 2.
    Includes independent primary/backup arrays + header CRCs. Files only."""
    raw=Path(raw); out=Path(out)
    fat_start=34816 # 1 MiB alignment + 16 MiB Microsoft reserved partition
    fat_size=raw.stat().st_size//512
    total=fat_start+fat_size+2048
    assert stride>=128 and count>fat_slot>=1
    entries=bytearray(count*stride)
    array_sectors=(len(entries)+511)//512
    def entry(index,kind,start,end,label):
        off=index*stride
        entries[off:off+16]=uuid.UUID(kind).bytes_le
        entries[off+16:off+32]=uuid.UUID(int=index+12345).bytes_le
        struct.pack_into('<QQQ',entries,off+32,start,end,0)
        data=label.encode('utf-16le')[:72];entries[off+56:off+56+len(data)]=data
    entry(0,'e3c9e316-0b5c-4db8-817d-f92df00215ae',2048,fat_start-1,'Microsoft reserved')
    entry(fat_slot,'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7',fat_start,fat_start+fat_size-1,'KVANTIMPORT')
    table_crc=zlib.crc32(entries)
    def header(at,other,array):
        h=bytearray(512)
        struct.pack_into('<8sIIIIQQQQ16sQIII',h,0,b'EFI PART',0x10000,92,0,0,
                         at,other,2+array_sectors,total-array_sectors-2,uuid.UUID(int=54321).bytes_le,array,count,stride,table_crc)
        struct.pack_into('<I',h,16,zlib.crc32(h[:92]))
        return h
    mbr=bytearray(512)
    struct.pack_into('<B3sB3sII',mbr,446,0,b'\0'*3,0xEE,b'\xff'*3,1,total-1)
    mbr[510:512]=b'\x55\xaa'
    with out.open('wb') as f,raw.open('rb') as src:
        f.truncate(total*512);f.seek(0);f.write(mbr)
        f.seek(512);f.write(header(1,total-1,2));f.write(entries)
        f.seek(fat_start*512)
        while chunk:=src.read(1024*1024):f.write(chunk)
        f.seek((total-1-array_sectors)*512);f.write(entries);f.write(header(total-1,1,total-1-array_sectors))
    return out
if __name__=='__main__':
    raw,mbr,_=make(sys.argv[1]);print(raw);print(mbr);print(make_gpt(raw,Path(sys.argv[1])/'flash-gpt.img'))
