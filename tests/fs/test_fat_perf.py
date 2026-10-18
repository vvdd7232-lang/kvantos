#!/usr/bin/env python3
"""Real FAT/VFS/WAL call-count regression; private 64 MiB regular-file image.
Optional FAT_PERF_BASELINE points at this harness linked with checkpoint fat.c.
Counts are host block calls, NOT USB commands or a wall-time qualification.
"""
from pathlib import Path
import os,re,subprocess,sys
ROOT=Path(__file__).resolve().parents[2]
W=ROOT/'build/fat-perf';W.mkdir(parents=True,exist_ok=True)
IMG=W/'test.img';TOTAL=131072
EXE=ROOT/'tests/fs/build/fat-owned'
def run(*a):
    p=subprocess.run([str(x) for x in a],capture_output=True,text=True)
    assert p.returncode==0,(a,p.returncode,p.stdout,p.stderr)
    return p.stdout
def fresh(listing=False):
    with IMG.open('wb') as f:f.truncate(TOTAL*512)
    run('/sbin/mkfs.fat','-F32','-s1','-n','FATPERF','-i','12345678',IMG)
    old=W/'old.txt';old.write_bytes(b'O'*1000)
    run('mcopy','-i',IMG,old,'::Old long name.txt')
    run('mmd','-i',IMG,'::Empty long directory')
    if listing:
        for i in range(16):run('mcopy','-i',IMG,old,f'::Long listing entry number {i:02}.txt')
    run(sys.executable,ROOT/'tools/fat-wal.py',IMG,'--prepare','--confirm','12345678')
def measure(exe):
    fresh();stats=[]
    for op in ('large','read-large'):
        out=run(exe,IMG,0,TOTAL,op);print(f'{Path(exe).name}/{op}: {out.strip()}',flush=True)
        assert 'result=65535 ' in out,out
        stats.append(tuple(map(int,re.search(r'reads=(\d+) writes=(\d+) syncs=(\d+)',out).groups())))
    run(exe,IMG,0,TOTAL,'mount');run('/sbin/fsck.fat','-n',IMG)
    out=W/'large.bin';out.unlink(missing_ok=True);run('mcopy','-i',IMG,'::Large.txt',out)
    assert out.read_bytes()==bytes((i*19+7)&255 for i in range(65535))
    return stats
current=measure(EXE)
assert current[0][0]<256 and current[0][1]<192,current
assert current[1][0]<160 and current[1][1]==0,current
if baseline:=os.environ.get('FAT_PERF_BASELINE'):
    previous=measure(baseline)
    assert previous[0][0]>4*current[0][0] and previous[0][1]>4*current[0][1],(previous,current)
    print('PASS checkpoint comparison: >4x fewer write-operation reads AND writes',flush=True)
print('PASS bounded host calls, exact 65535 bytes, cold reopen and independent fsck/mcopy',flush=True)

# Multi-cluster directory listing must retain the FAT read scope inside walk_dir.
def listing(exe,op="list"):
    fresh(listing=True);out=run(exe,IMG,0,TOTAL,op);print(f'{Path(exe).name}/{op}: {out.strip()}',flush=True)
    m=re.search(r'result=(\d+).*reads=(\d+) writes=(\d+)',out)
    assert m and int(m[1])>=18 and int(m[3])==0,out
    return int(m[2])
reads=listing(EXE)
if baseline:=os.environ.get('FAT_PERF_BASELINE'):
    assert listing(baseline)>reads*1.4
print('PASS operation-scoped FAT links also reduce multi-cluster directory listing I/O',flush=True)

reads=listing(EXE,'list-batch');assert reads<20
out=run(EXE,IMG,0,TOTAL,'list-check');assert 'result=19 ' in out,out
if baseline:=os.environ.get('FAT_PERF_BASELINE'):
    assert listing(baseline,'list-batch')>reads*8
print('PASS bounded batch listing: exact single-entry equivalence, offsets, end-of-directory, invalid requests, file refusal; no writes',flush=True)

# An error in any batch read is not an end-of-directory or a partial success.
before=IMG.read_bytes()
for cut in range(1,reads+1):
    out=run(EXE,IMG,0,TOTAL,'list-batch','read-eio',cut)
    assert 'result=-1 ' in out and 'tripped=1 ' in out,out
    assert IMG.read_bytes()==before
run(EXE,IMG,0,TOTAL,'mount');run('/sbin/fsck.fat','-n',IMG)
print(f'PASS {reads} batch read-EIO cuts: no partial success, exact whole-image unchanged, cold fsck',flush=True)
