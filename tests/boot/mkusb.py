#!/usr/bin/env python3
"""Private FAT32 fixtures via dosfstools/mtools; never formats a host device."""
from pathlib import Path
import os, struct, subprocess, sys

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
if __name__=='__main__':
    raw,mbr,_=make(sys.argv[1]);print(raw);print(mbr)
