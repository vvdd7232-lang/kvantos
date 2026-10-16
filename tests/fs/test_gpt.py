#!/usr/bin/env python3
"""GPT parser checks with the real FAT/VFS/block drivers and read-only USB stub.
Only regular images in build/ are touched. No mounts and no host devices.
"""
from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, zlib
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/boot'))
from mkusb import make,make_gpt
WORK=ROOT/'tests/fs/build/gpt'
WORK.mkdir(parents=True,exist_ok=True)
HARNESS=ROOT/'tests/fs/build/harness'
raw,mbr,_=make(WORK)
base=make_gpt(raw,WORK/'flash-gpt.img')
passed=0

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        while data:=f.read(1024*1024):h.update(data)
    return h.hexdigest()
def run(path,valid,label,slot=None):
    global passed
    before=digest(path)
    result=subprocess.run([str(HARNESS),str(path),'--usb' if valid else '--usb-empty'],text=True,capture_output=True)
    assert result.returncode==0,(label,result.stdout,result.stderr)
    if slot is not None:assert f'/mnt/usb0p{slot} ' in result.stdout,(label,result.stdout)
    assert before==digest(path),(label,'source image modified')
    passed+=1;print('PASS',label,flush=True)
def variant(label,change,valid):
    p=WORK/'mutated.img';shutil.copyfile(base,p)
    with p.open('r+b') as f:change(f)
    run(p,valid,label);p.unlink()
def headers(f,change):
    total=base.stat().st_size//512
    for at in (1,total-1):
        f.seek(at*512);h=bytearray(f.read(512));change(h)
        struct.pack_into('<I',h,16,0)
        struct.pack_into('<I',h,16,zlib.crc32(h[:92]))
        f.seek(at*512);f.write(h)
def xor(f,at):
    f.seek(at);b=f.read(1);f.seek(at);f.write(bytes([b[0]^1]))
def entries(f,change):
    total=base.stat().st_size//512
    f.seek(2*512);data=bytearray(f.read(16384));change(data)
    for lba in (2,total-33):f.seek(lba*512);f.write(data)
    headers(f,lambda h:struct.pack_into('<I',h,88,zlib.crc32(data)))

run(raw,True,'raw FAT32 regression')
run(mbr,True,'MBR FAT32 regression',1)
run(base,True,'Windows-like GPT with MSR + FAT32 in slot 2, 24 rescans',2)
validator=shutil.which('sgdisk') or '/sbin/sgdisk'
if Path(validator).exists():
    check=subprocess.run([validator,'-v',str(base)],text=True,capture_output=True,check=True)
    assert 'No problems found' in check.stdout,check.stdout
    passed+=1;print('PASS independent sgdisk validation',flush=True)
variant('bad primary header CRC: use backup',lambda f:xor(f,512+16),True)
variant('bad primary array CRC: use backup',lambda f:xor(f,2*512+15),True)
variant('missing primary signature: use backup',lambda f:xor(f,512),True)
variant('bad backup ignored while primary is valid',lambda f:xor(f,base.stat().st_size-512+16),True)
variant('both header CRCs invalid: refuse',lambda f:[xor(f,at) for at in (512+16,base.stat().st_size-512+16)],False)
variant('both array CRCs invalid: refuse',lambda f:[xor(f,at*512+15) for at in (2,base.stat().st_size//512-33)],False)
variant('64-bit entry-array LBA beyond media: refuse',lambda f:headers(f,lambda h:struct.pack_into('<Q',h,72,1<<32)),False)
variant('huge entry count: bounded refusal',lambda f:headers(f,lambda h:struct.pack_into('<I',h,80,0xFFFFFFFF)),False)
variant('invalid entry stride: refuse',lambda f:headers(f,lambda h:struct.pack_into('<I',h,84,96)),False)
variant('array overlaps usable data: refuse',lambda f:headers(f,lambda h:struct.pack_into('<Q',h,40,2)),False)
variant('reversed usable range: refuse',lambda f:headers(f,lambda h:struct.pack_into('<Q',h,48,1)),False)
variant('partition end before start: skip',lambda f:entries(f,lambda a:struct.pack_into('<Q',a,128+40,34815)),False)
variant('partition start above 32-bit LBA: skip',lambda f:entries(f,lambda a:struct.pack_into('<Q',a,128+32,1<<32)),False)
variant('partition end outside usable range: skip',lambda f:entries(f,lambda a:struct.pack_into('<Q',a,128+40,base.stat().st_size//512)),False)
for stride,count,slot in ((256,128,1),(512,128,1),(1024,128,1),(4096,32,31),(128,1024,1023)):
    p=make_gpt(raw,WORK/'wide-gpt.img',stride,count,slot)
    run(p,True,f'GPT entry stride {stride}, count {count}, FAT slot {slot+1}',slot+1)
    p.unlink()
print(f'{passed} GPT/MBR/raw checks passed; all source-image hashes unchanged',flush=True)
