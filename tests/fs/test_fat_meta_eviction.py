#!/usr/bin/env python3
"""Actual FAT/WAL dirty eviction with fragmented free space; private files only.
Spaced cuts, not an exhaustive fault matrix (test_fat_owned.py supplies that).
"""
from pathlib import Path
import hashlib,os,re,shutil,struct,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];W=ROOT/'build/fat-meta-eviction';W.mkdir(parents=True,exist_ok=True)
IMG=W/'test.img';SNAP=W/'before.img';TOTAL=131072
EXE=Path(os.environ.get('FAT_OWNED_HARNESS',str(ROOT/'tests/fs/build/fat-owned')))
def run(*a,ok=(0,),trace=False):
    p=subprocess.run([str(x) for x in a],capture_output=True,text=True,
                     env=dict(os.environ,FAT_TRACE='1') if trace else os.environ)
    assert p.returncode in ok,(a,p.returncode,p.stdout,p.stderr)
    return p.stdout
def invoke(op,*a,**kw):return run(EXE,IMG,0,TOTAL,op,*a,**kw)
def fsck():run('/sbin/fsck.fat','-n',IMG)
def namespace():
    out=W/'export';shutil.rmtree(out,ignore_errors=True);out.mkdir()
    run('mcopy','-s','-i',IMG,'::*',out)
    return {p.relative_to(out).as_posix():None if p.is_dir() else hashlib.sha256(p.read_bytes()).hexdigest()
            for p in out.rglob('*') if p.name.upper()!='KVANTWAL.SYS'}
with IMG.open('wb') as f:f.truncate(TOTAL*512)
run('/sbin/mkfs.fat','-F32','-s1','-i','12345678',IMG)
old=W/'old.txt';old.write_bytes(b'O'*1000);run('mcopy','-i',IMG,old,'::Old long name.txt')
# Allocate the WAL before punching holes: it must not consume the test holes.
run(sys.executable,ROOT/'tools/fat-wal.py',IMG,'--prepare','--confirm','12345678')
hole=W/'hole.bin';hole.write_bytes(b'H'*512)
pad=W/'pad.bin';pad.write_bytes(b'P'*131072)
for i in range(12):
    run('mcopy','-i',IMG,hole,f'::H{i:02}.BIN');run('mcopy','-i',IMG,pad,f'::P{i:02}.BIN')
for i in range(12):run('mdel','-i',IMG,f'::H{i:02}.BIN')
fsck();before=namespace();shutil.copyfile(IMG,SNAP)
out=invoke('large',trace=True);assert 'result=65535 ' in out,out
steps=int(re.search(r'steps=(\d+)',out)[1])
with IMG.open('rb') as f:bpb=f.read(512)
reserved=struct.unpack_from('<H',bpb,14)[0];fatsz=struct.unpack_from('<I',bpb,36)[0]
touched={int(lba) for lba in re.findall(r'write=\d+ lba=(\d+)',out)
         if reserved<=int(lba)<reserved+2*fatsz}
assert len(touched)>8,(len(touched),out)
invoke('mount');fsck();after=namespace();assert before!=after
invoke('read-large')
checks=0
cuts=sorted({1,steps,*range(1,steps+1,max(1,steps//12))})
for fault in ('before','after','eio','unplug'):
    for cut in cuts:
        shutil.copyfile(SNAP,IMG);invoke('large',fault,cut,ok=(0,77))
        invoke('mount');fsck();assert namespace() in (before,after),(fault,cut)
        checks+=1
print(f'PASS actual dirty eviction: {len(touched)} distinct FAT target sectors (>8 slots), {steps} write/barrier boundaries; {checks} spaced cuts, cold old-or-new namespace/fsck',flush=True)
