#!/usr/bin/env python3
"""Normal (no lab/test hooks) FAT -> file WAL; private images, no host devices.
Exhaustive process-kill/EIO/recovery cuts, independent fsck/namespace checks,
invalid ownership/aliases/crosslinks and zero writes on failed enrollment.
"""
from pathlib import Path
import importlib.util, os, struct, subprocess, re, hashlib, shutil, sys, zlib
ROOT=Path(__file__).resolve().parents[2]
W=ROOT/'build/fat-owned-host';W.mkdir(parents=True,exist_ok=True)
EXE=Path(os.environ.get('FAT_OWNED_HARNESS',str(ROOT/'tests/fs/build/fat-owned')))
TOOL=ROOT/'tools/fat-wal.py';IMG=W/'test.img';TOTAL=131072
spec=importlib.util.spec_from_file_location('wal',TOOL);wal=importlib.util.module_from_spec(spec);spec.loader.exec_module(wal)

def run(*a,ok=(0,)):
    p=subprocess.run([str(x) for x in a],capture_output=True,text=True)
    assert p.returncode in ok,(a,p.returncode,p.stdout,p.stderr)
    return p

def invoke(op,*args,ok=(0,),first=0): return run(EXE,IMG,first,TOTAL,op,*args,ok=ok)
def sparse(path):
    out=[]
    with path.open('rb') as f:
        while b:=f.read(65536):
            if b.strip(b'\0'):out.append((f.tell()-len(b),b))
    return path.stat().st_size,out

def clone(snap=None):
    size,chunks=snapshot if snap is None else snap
    with IMG.open('wb') as f:
        f.truncate(size)
        for at,b in chunks:f.seek(at);f.write(b)

def digest():
    h=hashlib.sha256()
    with IMG.open('rb') as f:
        while b:=f.read(1048576):h.update(b)
    return h.digest()
def fsck(): run('/sbin/fsck.fat','-n',IMG)
def namespace():
    out=W/'export'
    if out.exists():shutil.rmtree(out)
    out.mkdir();run('mcopy','-s','-i',IMG,'::*',out)
    return {p.relative_to(out).as_posix():('dir' if p.is_dir() else hashlib.sha256(p.read_bytes()).hexdigest())
            for p in out.rglob('*') if p.name.upper()!='KVANTWAL.SYS'}
def patch(at,b):
    with IMG.open('r+b') as f:f.seek(at);f.write(b)

def reject(label):
    before=digest();p=invoke('mount',ok=(4,));assert 'writes=0' in p.stdout,p.stdout
    assert digest()==before,label
    print('PASS reject/no writes:',label,flush=True)

with IMG.open('wb') as f:f.truncate(TOTAL*512)
run('/sbin/mkfs.fat','-F32','-s1','-n','OWNEDM2','-i','12345678',IMG)
old=W/'old.txt';old.write_bytes(b'O'*1000)
run('mcopy','-i',IMG,old,'::Old long name.txt');run('mmd','-i',IMG,'::Empty long directory')
for i in range(7):run('mcopy','-i',IMG,old,f'::F{i:02}.TXT')
snapshot=sparse(IMG);before=digest();invoke('probe');assert digest()==before
p=run(sys.executable,TOOL,IMG);assert 'No owned WAL' in p.stdout and digest()==before
run(sys.executable,TOOL,IMG,'--prepare','--confirm','DEADBEEF',ok=(1,));assert digest()==before
print('PASS normal default RO, readable after denied opt-in, inspect/wrong confirmation write nothing',flush=True)
run(sys.executable,TOOL,IMG,'--prepare','--confirm','12345678');fsck();snapshot=sparse(IMG)
fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL);owner=v.owner();wal_sector,fields,row=owner;os.close(fd)
for op in ('disable','stale','guard'):clone();invoke(op);fsck()
clone();before=digest();run(sys.executable,TOOL,IMG,'--prepare','--confirm','12345678',ok=(1,));assert digest()==before
old_namespace=namespace();cases=0
for op in ('overwrite','empty','create','mkdir','remove','rmdir','large'):
    clone();p=invoke(op);m=re.search(r'result=(-?\d+) steps=(\d+)',p.stdout)
    assert m and int(m[1])>=0,p.stdout
    steps=int(m[2]);invoke('mount');fsck();new_namespace=namespace()
    for fault in ('before','after','eio','unplug'):
        # 64 KiB: every boundary too, including many FAT allocation/data cuts.
        for cut in range(1,steps+1):
            clone();p=invoke(op,fault,cut,ok=(0,77))
            if fault in ('before','after'):assert p.returncode==77,(op,fault,cut,p.stdout)
            else:assert 'tripped=1' in p.stdout,p.stdout
            invoke('mount');fsck();assert namespace() in (old_namespace,new_namespace),(op,fault,cut)
            cases+=1
        print(f'PASS owned WAL {op}/{fault}: {steps} cuts, cold recovery + namespace + fsck',flush=True)
print(f'PASS {cases} interrupted normal-policy owned-WAL operations',flush=True)
# Every target/header/backup write is torn, not only clean before/after cuts.
clone();p=run('env','FAT_TRACE=1',EXE,IMG,0,TOTAL,'overwrite')
writes=[(int(a),int(b)) for a,b in re.findall(r'write=(\d+) lba=(\d+)',p.stdout)]
for cut,lba in writes:
    clone();invoke('overwrite','torn',cut,ok=(77,))
    if lba==wal_sector+1: reject(f'torn WAL header boundary {cut}')
    else:invoke('mount');fsck();assert namespace()==old_namespace,(cut,lba)
print(f'PASS {len(writes)} torn-sector writes: target undo / ambiguous WAL header fenced',flush=True)
# Recovery itself can be interrupted repeatedly. Build one mid-publication log.
clone();invoke('overwrite','before',writes[-2][0],ok=(77,));pending=sparse(IMG)
p=invoke('mount');steps=int(re.search(r'steps=(\d+)',p.stdout)[1]);assert steps>0
for fault in ('before','after','eio','unplug'):
    for cut in range(1,steps+1):
        clone(pending);invoke('mount',fault,cut,ok=(4,77));invoke('mount');fsck();assert namespace()==old_namespace
print(f'PASS {steps*4} recovery-interruption cuts; idempotent before namespace publication',flush=True)
# Read failure cuts during mutation must not be mistaken for missing files.
for op in ('overwrite','read','list'):
    clone();p=invoke(op,'read-eio',0);reads=int(re.search(r'steps=(\d+)',p.stdout)[1])
    for cut in range(1,reads+1):
        clone();invoke(op,'read-eio',cut);invoke('mount');fsck();assert namespace()==old_namespace
    print(f'PASS {reads} injected read errors: {op}',flush=True)
# Inbound FAT references (even lost chains) and directory aliases into ANY
# owned cluster are rejected BEFORE a write, not just protected by filename.
for relative,description in ((0,'first'),(fields[6]-1,'last')):
    clone();fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL)
    free=next(c for c in range(2,v.clusters+2) if not v.fat(c));os.close(fd)
    for copy in range(2):patch((v.reserved+copy*v.fatsz)*512+free*4,struct.pack('<I',fields[5]+relative))
    reject('foreign FAT reference into '+description+' owned cluster')
    clone()
    # Change an ordinary file's starting cluster, leaving its name/size intact.
    fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL)
    target=next(r for r in v.entries(v.root) if r[2][:11]==b'F00     TXT');os.close(fd)
    d=bytearray(target[2]);c=fields[5]+relative;struct.pack_into('<H',d,20,c>>16);struct.pack_into('<H',d,26,c&65535)
    patch(target[0]*512+target[1],d);reject('directory alias into '+description+' owned cluster')
for at,label in ((wal_sector*512+100,'owner CRC'),((wal_sector+1)*512+100,'WAL CRC'),(67,'BPB identity'),
                 (row[0]*512+row[1]+11,'owner attributes')):
    clone();patch(at,b'X');reject(label)
clone();patch(v.reserved*512+40,b'X');reject('unexplained FAT mirror mismatch')
# Correct CRC is not permission to overwrite owner/file/boot/other-volume LBAs.
for target in (0,wal_sector,wal_sector+1,TOTAL,0xFFFFFFFF):
    clone();h=bytearray(512);h[:8]=b'KVSTX01\0'
    struct.pack_into('<8I',h,8,1,fields[2],0,TOTAL,wal.VIRTUAL,49,1,1)
    backup=bytes(512);struct.pack_into('<2I',h,40,target,zlib.crc32(backup));struct.pack_into('<I',h,508,zlib.crc32(h[:508]))
    patch((wal_sector+2)*512,backup);patch((wal_sector+1)*512,h);reject(f'forbidden undo target {target}')
# Whole-volume space exhaustion is rolled back without touching old contents.
clone();fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL);free=wal.U32(v.read(wal.U16(v.bpb,48)),488);os.close(fd)
filler=W/'fill.bin'
with filler.open('wb') as f:f.truncate(free*512)
run('mcopy','-i',IMG,filler,'::FILL.BIN');fsck();full=namespace();p=invoke('overwrite')
assert 'result=-3 ' in p.stdout,p.stdout
invoke('mount');fsck();assert namespace()==full;filler.unlink()
print('PASS full volume, old file and complete namespace preserved',flush=True)
# Different cluster sizes, relative WAL relocation to an ordinary GPT partition,
# and tool offset mode without writes anywhere outside the FAT filesystem.
for spc in (2,8,64):
    with IMG.open('wb') as f:f.truncate(524288*512)
    # >=65525 clusters is mandatory AFTER reserved/FAT overhead.
    if spc==8:
        with IMG.open('wb') as f:f.truncate(2097152*512)
    if spc==64:
        with IMG.open('wb') as f:f.truncate(8388608*512)
    run('/sbin/mkfs.fat','-F32',f'-s{spc}','-i','12345678',IMG)
    run(sys.executable,TOOL,IMG,'--prepare','--confirm','12345678');fsck()
    total=IMG.stat().st_size//512
    # mkfs can round total sectors down; obtain the authoritative BPB count.
    with IMG.open('rb') as f:bpb=f.read(512)
    total=wal.U32(bpb,32)
    run(EXE,IMG,0,total,'create');run(EXE,IMG,0,total,'mount');fsck()
    print(f'PASS cluster size {spc*512} bytes (rounded owned file extent)',flush=True)
# Small image back for actual GPT/MBR embedding; private regular files only.
clone();raw=W/'raw.img';shutil.copyfile(IMG,raw)
sys.path.insert(0,str(ROOT/'tests/boot'));from mkusb import make_gpt
make_gpt(raw,IMG);first=34816
with IMG.open('rb') as f:outside=(f.read(first*512),);f.seek((first+TOTAL)*512);outside+= (f.read(),)
run(sys.executable,TOOL,IMG,'--start',first)
invoke('create',first=first);invoke('mount',first=first)
with IMG.open('rb') as f:assert f.read(first*512)==outside[0];f.seek((first+TOTAL)*512);assert f.read()==outside[1]
print('PASS GPT-relative WAL, cold reopen; both GPT copies/MSR/gaps byte-identical',flush=True)
print('PASS all normal-policy owned-WAL software image checks (not physical USB proof)',flush=True)
