#!/usr/bin/env python3
"""Normal owned-file WAL fixture; private regular files ONLY, no lab manifest."""
from pathlib import Path
import importlib.util, os, struct, subprocess, sys
from mkusb import make_gpt
ROOT=Path(__file__).resolve().parents[2]
TOTAL=131072

def make(out,layout):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    raw=out/'fat-part.img'
    with raw.open('wb') as f:f.truncate(TOTAL*512)
    def tool(*a):subprocess.run([str(x) for x in a],check=True,capture_output=True)
    tool('/sbin/mkfs.fat','-F32','-s1','-n','M24OWNED','-i','12345678',raw)
    old=out/'old.txt';old.write_bytes(b'O'*1000)
    tool('mcopy','-i',raw,old,'::Old long name.txt');tool('mmd','-i',raw,'::Empty long directory')
    for i in range(7):tool('mcopy','-i',raw,old,f'::F{i:02}.TXT')
    tool(sys.executable,ROOT/'tools/fat-wal.py',raw,'--prepare','--confirm','12345678')
    spec=importlib.util.spec_from_file_location('fat_wal_tool',ROOT/'tools/fat-wal.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    fd=os.open(raw,os.O_RDONLY)
    try:
        v=module.Volume(fd,0,TOTAL);owner=v.owner();relative_owner=owner[0];bpb=v.bpb
    finally:os.close(fd)
    path=out/'flash.img'
    if layout=='gpt':make_gpt(raw,path);first=34816
    else:
        first=2048;mbr=bytearray(512)
        struct.pack_into('<B3sB3sII',mbr,446,0,b'\0'*3,0x0c,b'\0'*3,first,TOTAL);mbr[510:512]=b'\x55\xaa'
        with path.open('wb') as f,raw.open('rb') as src:
            f.truncate((first+TOTAL+2048)*512);f.write(mbr);f.seek(first*512)
            while b:=src.read(65536):f.write(b)
    return path,first,TOTAL,first+relative_owner+1,bpb
