#!/usr/bin/env python3
"""Explicit ownership manifest + preformatted WAL in our own regular files.
Not a preparation utility for physical devices. No guest ever initializes a log.
"""
from pathlib import Path
import struct,subprocess,zlib
from mkusb import make_gpt
TOTAL=131072
MAGIC=b'KVANTOS-M2-FAT-LAB\0'
def make(out,layout):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    raw=out/'fat-part.img'
    with raw.open('wb') as f:f.truncate(TOTAL*512)
    subprocess.run(['/sbin/mkfs.fat','-F','32','-s','1','-n','M22BLAB',str(raw)],check=True,stdout=subprocess.DEVNULL)
    old=out/'old.txt';old.write_bytes(b'O'*1000)
    def tool(*a):subprocess.run(a,check=True,stdout=subprocess.DEVNULL)
    tool('mcopy','-i',str(raw),str(old),'::Old long name.txt')
    tool('mmd','-i',str(raw),'::Empty long directory')
    for i in range(7):tool('mcopy','-i',str(raw),str(old),f'::F{i:02}.TXT')
    path=out/'flash.img'
    if layout=='gpt':make_gpt(raw,path);first=34816
    else:
        first=2048;mbr=bytearray(512)
        struct.pack_into('<B3sB3sII',mbr,446,0,b'\0'*3,0x0c,b'\0'*3,first,TOTAL);mbr[510:512]=b'\x55\xaa'
        with path.open('wb') as f,raw.open('rb') as src:
            f.truncate((first+TOTAL+2048)*512);f.write(mbr);f.seek(first*512)
            while b:=src.read(65536):f.write(b)
    capacity=path.stat().st_size//512;marker=capacity-1024;journal=marker+1
    with raw.open('rb') as f:bpb=f.read(512)
    serial=struct.unpack_from('<I',bpb,67)[0]
    owner=bytearray(512);owner[:len(MAGIC)]=MAGIC
    struct.pack_into('<8I',owner,64,capacity,2,first,TOTAL,serial,journal,49,zlib.crc32(bpb))
    struct.pack_into('<I',owner,508,zlib.crc32(owner[:508]))
    log=bytearray(512);log[:8]=b'KVSTX01\0'
    struct.pack_into('<8I',log,8,1,serial,first,TOTAL,journal,49,0,0)
    struct.pack_into('<I',log,508,zlib.crc32(log[:508]))
    with path.open('r+b') as f:
        f.seek(marker*512);assert not f.read(50*512).strip(b'\0')
        f.seek(marker*512);f.write(owner);f.write(log)
    return path,first,TOTAL,journal,bpb
