#!/usr/bin/env python3
"""Actual FAT/VFS on disposable sparse files: crash/restart + independent fsck.
No mount, raw device, implicit journal gap or physical USB permission.
Every write/barrier boundary is interrupted in a fresh process.
"""
from pathlib import Path
import os, struct, subprocess, re, hashlib, shutil
W=Path(__file__).resolve().parent/'build/fat-write'
W.mkdir(parents=True,exist_ok=True)
EXE=Path(os.environ.get('FAT_HARNESS',str(W.parent/'fat-fault'))).resolve(); BASE=W/'base.img'; IMG=W/'test.img'; PART=W/'part.img'
TOTAL=131072
MKFS=shutil.which('mkfs.fat') or '/sbin/mkfs.fat'
FSCK=shutil.which('fsck.fat') or '/sbin/fsck.fat'
def run(*args,ok=(0,)):
    p=subprocess.run([str(a) for a in args],capture_output=True,text=True)
    assert p.returncode in ok,(args,p.returncode,p.stdout,p.stderr)
    return p

def invoke(mode,*args,ok=(0,)):
    return run(EXE,IMG,TOTAL,mode,*args,ok=ok)

def clone(chunks=None):
    chunks=chunks if chunks is not None else snapshot
    with IMG.open('wb') as f:
        f.truncate((TOTAL+49)*512)
        for at,data in chunks:f.seek(at);f.write(data)

def sparse(path):
    out=[]
    with path.open('rb') as f:
        while b:=f.read(65536):
            at=f.tell()-len(b)
            if b.strip(b'\0'):out.append((at,b))
    return out

def fsck():
    # fsck accepts an appended journal but -n must not modify either area.
    stamp=IMG.stat().st_mtime_ns
    p=run(FSCK,'-n',IMG)
    assert 'Leaving filesystem unchanged' not in p.stdout,p.stdout
    assert IMG.stat().st_mtime_ns==stamp

with BASE.open('wb') as f:f.truncate(TOTAL*512)
run(MKFS,'-F','32','-s','1','-n','M22TEST',BASE)
old=W/'old.txt';old.write_bytes(b'O'*1000)
run('mcopy','-i',BASE,old,'::Old long name.txt')
run('mmd','-i',BASE,'::Empty long directory')
# Force a non-contiguous directory chain, with a free tail slot where a new
# long name must span into a freshly allocated, physically distant cluster.
for i in range(7):run('mcopy','-i',BASE,old,f'::F{i:02}.TXT')
with BASE.open('r+b') as f:f.truncate((TOTAL+49)*512)
snapshot=sparse(BASE);clone();invoke('init');snapshot=sparse(IMG)
def namespace():
    out=W/'export'
    if out.exists():shutil.rmtree(out)
    out.mkdir()
    run('mcopy','-s','-i',IMG,'::*',out)
    return {p.relative_to(out).as_posix():('dir' if p.is_dir() else hashlib.sha256(p.read_bytes()).hexdigest()) for p in out.rglob('*')}
fsck();old_namespace=namespace()
print('PASS explicit journal provision + independent baseline fsck',flush=True)
cases=0
for op in ('overwrite','empty','create','mkdir','remove','rmdir','large'):
    clone();p=invoke('attached',op);m=re.search(r'result=(-?\d+) steps=(\d+)',p.stdout)
    assert m and int(m[1])>=0,(op,p.stdout)
    steps=int(m[2]);invoke('recover');fsck();new_namespace=namespace()
    if op=='overwrite':invoke('attached','check')
    print(f'PASS {op}: complete, reopen and fsck; {steps} write/barrier boundaries',flush=True)
    # Keep the 64 KiB case for successful multicluster regression; the smaller
    # replacement already exercises each protocol stage exhaustively.
    if op=='large':continue
    for fault in ('before','after','eio','unplug'):
        for cut in range(1,steps+1):
            clone();p=invoke('attached',op,fault,cut,ok=(0,77))
            if fault in ('before','after'):assert p.returncode==77,(op,fault,cut,p.stdout)
            else:assert 'tripped=1' in p.stdout,(op,fault,cut,p.stdout)
            invoke('recover');fsck()
            assert namespace() in (old_namespace,new_namespace),(op,fault,cut,'neither old nor new namespace')
            if op in ('overwrite','empty'):invoke('attached','check')
            cases+=1
        print(f'PASS {op}: {fault}, {steps} fresh-process recoveries + fsck',flush=True)
print(f'PASS {cases} interrupted FAT operations; no false success or post-recovery fsck errors',flush=True)
# Invalid journal must fence without guessing or writing to any sector.
clone()
with IMG.open('r+b') as f:f.seek(TOTAL*512+100);f.write(b'X')
before=hashlib.sha256(IMG.read_bytes()).digest()
invoke('recover',ok=(4,))
assert hashlib.sha256(IMG.read_bytes()).digest()==before
print('PASS corrupted journal header: fail closed, entire image unchanged',flush=True)
# Provisioning must never overwrite a live/dirty log.
clone();before=hashlib.sha256(IMG.read_bytes()).digest();invoke('init',ok=(4,))
assert hashlib.sha256(IMG.read_bytes()).digest()==before
print('PASS refuse to reformat an existing journal',flush=True)
# Partial target-sector writes are recoverable. Partial WAL headers are NOT
# guessed at: invalid CRC means fail-closed, preserving the entire image.
clone();p=run('env','FAT_TRACE=1',EXE,IMG,TOTAL,'attached','overwrite')
writes=[(int(a),int(b)) for a,b in re.findall(r'write=(\d+) lba=(\d+)',p.stdout)]
for cut,lba in writes:
    clone();invoke('attached','overwrite','torn',cut,ok=(77,))
    if lba==TOTAL:
        before=hashlib.sha256(IMG.read_bytes()).digest();invoke('recover',ok=(4,))
        assert hashlib.sha256(IMG.read_bytes()).digest()==before
    else:
        invoke('recover');fsck();assert namespace()==old_namespace
print(f'PASS {len(writes)} torn-sector cuts: target undo or invalid-header fence',flush=True)
# All read errors: metadata errors must not look like end-of-chain/missing
# entries; a data read error must not return a successful partial document.
for op in ('overwrite','read'):
    clone();p=invoke('attached',op,'read-eio',0)
    reads=int(re.search(r'steps=(\d+)',p.stdout)[1])
    for cut in range(1,reads+1):
        clone();invoke('attached',op,'read-eio',cut);invoke('recover');fsck()
        assert namespace()==old_namespace
    print(f'PASS {op}: {reads} injected read errors, old namespace intact',flush=True)
# Normal kernels use the same COW/metadata undo with no persistent WAL.
for op in ('overwrite','create','mkdir','remove','rmdir'):
    clone();p=invoke('ram',op);steps=int(re.search(r'steps=(\d+)',p.stdout)[1])
    fsck()
    for cut in range(1,steps+1):
        clone();invoke('ram',op,'eio',cut);fsck();assert namespace()==old_namespace
    print(f'PASS normal-kernel RAM undo: {op}, {steps} handled write/barrier errors',flush=True)
for op in ('dirwrite','badname','badutf8','badparent'):
    clone();before=hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()
    p=invoke('attached',op);assert 'result=-' in p.stdout,p.stdout
    assert hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()==before
    fsck()
print('PASS refuse directory overwrite, invalid UTF-8/name syntax and non-directory parent',flush=True)
clone();run('mattrib','-i',IMG,'+r','::Old long name.txt')
before=hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()
p=invoke('attached','overwrite');assert 'result=-2 ' in p.stdout,p.stdout
assert hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()==before
fsck();print('PASS read-only attribute: refuse before allocating or altering any FAT sector',flush=True)
# Fill every remaining free cluster through somebody else's implementation.
clone()
with IMG.open('rb') as f:
    f.seek(512+488);free=struct.unpack('<I',f.read(4))[0]
filler=W/'fill.bin'
with filler.open('wb') as f:f.truncate(free*512)
run('mcopy','-i',IMG,filler,'::FILL.BIN');fsck()
before=hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()
p=invoke('attached','overwrite');assert 'result=-3 ' in p.stdout,p.stdout
invoke('recover');fsck();invoke('attached','check')
assert hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()==before
filler.unlink()
print('PASS completely full FAT32: old file/data/FAT/FSInfo/directory byte-identical',flush=True)
clone();invoke('attached','names');fsck()
assert {'A.B.C','A+B.TXT','A=B.TXT','A[B].TXT','Alias collision one.txt','Alias collision two.txt'} <= namespace().keys()
print('PASS long names with SFN-forbidden punctuation and colliding short aliases',flush=True)
# Exercise 512/4096-byte FAT16 sectors (block API remains 512-byte), without
# enabling the experimental FAT32 WAL for other formats.
for bps in (512,4096):
    with IMG.open('wb') as f:f.truncate(TOTAL*512)
    run(MKFS,'-F','16','-S',bps,IMG)
    run('mcopy','-i',IMG,old,'::Old long name.txt');run('mmd','-i',IMG,'::Empty long directory')
    invoke('ram','overwrite');invoke('ram','check');invoke('ram','large');invoke('ram','names')
    fsck()
    print(f'PASS FAT16 {bps}-byte sectors: COW replacement, 64 KiB, LFN + external fsck',flush=True)

# Recreate the FAT32 fixture for cross-cluster LFN deletion and RU casefold.
for create,remove in (('create','remove-new'),('mkdir','rmdir-new')):
    clone();invoke('attached',create)
    with IMG.open('rb') as f:
        f.seek(14);reserved=struct.unpack('<H',f.read(2))[0]
        f.seek(36);fat_size=struct.unpack('<I',f.read(4))[0]
        f.seek(44);root=struct.unpack('<I',f.read(4))[0]
        f.seek(reserved*512+root*4);nxt=struct.unpack('<I',f.read(4))[0]&0xFFFFFFF
        assert 2<=nxt<0xFFFFFF8 and nxt!=root+1
        f.seek((reserved+2*fat_size+root-2)*512+14*32+11);assert f.read(1)==b'\x0f'
    if create=='create':
        invoke('attached','ru-case');fsck()
        assert set(namespace())==set(old_namespace)|{'Новый длинный файл.txt'}
    invoke('attached',remove);fsck();assert namespace()==old_namespace
    print(f'PASS fragmented LFN {create}/{remove}: real cross-cluster slots, external namespace/fsck',flush=True)
clone()
# A damaged existing chain must be rejected, not silently shortened/repaired.
with IMG.open('r+b') as f:
    f.seek(14);reserved=struct.unpack('<H',f.read(2))[0]
    f.seek(36);fat_size=struct.unpack('<I',f.read(4))[0]
    for copy in range(2):f.seek((reserved+copy*fat_size)*512+3*4);f.write(struct.pack('<I',3))
before=hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()
p=invoke('attached','overwrite');assert 'result=-' in p.stdout
assert hashlib.sha256(IMG.read_bytes()[:TOTAL*512]).digest()==before
print('PASS cyclic old chain: bounded rejection, no filesystem repair during save',flush=True)
