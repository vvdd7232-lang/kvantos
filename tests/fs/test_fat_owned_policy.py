#!/usr/bin/env python3
"""Provisioner preflight/refusal and Linux physical-device policy unit tests.
ALL files/devices are simulated regular files under build/. No physical I/O.
"""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import hashlib,importlib.util,os,shutil,stat,struct,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];W=ROOT/'build/fat-owned-policy';W.mkdir(parents=True,exist_ok=True)
TOOL=ROOT/'tools/fat-wal.py'
spec=importlib.util.spec_from_file_location('wal_policy',TOOL);wal=importlib.util.module_from_spec(spec);spec.loader.exec_module(wal)
IMG=W/'target.img';TOTAL=131072

def run(*a,code=0):
    p=subprocess.run([str(x) for x in a],stdin=subprocess.DEVNULL,capture_output=True,text=True)
    assert p.returncode==code,(a,p.returncode,p.stdout,p.stderr)
    return p

def digest():
    h=hashlib.sha256()
    with IMG.open('rb') as f:
        while b:=f.read(1048576):h.update(b)
    return h.digest()
def fresh():
    with IMG.open('wb') as f:f.truncate(TOTAL*512)
    run('/sbin/mkfs.fat','-F32','-s1','-i','12345678',IMG)
def refused(*args):
    before=digest();p=run(sys.executable,TOOL,IMG,'--prepare','--confirm','12345678',*args,code=1)
    assert digest()==before,p.stdout
    return p

fresh();refused('--start','-1');refused('--start',TOTAL+1)
run(sys.executable,TOOL,'/dev/null','--prepare','--confirm','12345678',code=1)
print('PASS negative/out-of-range offset and non-disk special files refused without writes')
fresh();text=W/'text';text.write_bytes(b'not your journal')
run('mcopy','-i',IMG,text,'::KVANTWAL.SYS');refused()
print('PASS reserved filename collision never replaced')
fresh();names=[f'::D{i:04}' for i in range(1024)]
run('mmd','-i',IMG,*names);run('/sbin/fsck.fat','-n',IMG);refused()
print('PASS 1024 subdirectories + root exceed budget; preparation writes nothing')
# Completely full filesystem: staged mcopy fails, original is byte-identical.
fresh();fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL);free=wal.U32(v.read(wal.U16(v.bpb,48)),488);os.close(fd)
filler=W/'fill.bin'
with filler.open('wb') as f:f.truncate(free*512)
run('mcopy','-i',IMG,filler,'::FILL.BIN');run('/sbin/fsck.fat','-n',IMG);refused()
print('PASS full-volume preparation failure is staged; whole original unchanged')
# Turn FILL's one valid chain into a valid every-other-cluster fragmented file,
# returning every second cluster to free space. NO 50-cluster free run remains.
fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL)
entry=next(r for r in v.entries(v.root) if r[2][:11]==b'FILL    BIN');c=(wal.U16(entry[2],20)<<16)|wal.U16(entry[2],26)
chain=list(v.chain(c));keep,release=chain[::2],chain[1::2]
fat=bytearray(v.read(v.reserved,v.fatsz));os.close(fd)
for i,c in enumerate(keep):struct.pack_into('<I',fat,c*4,keep[i+1] if i+1<len(keep) else 0x0FFFFFFF)
for c in release:struct.pack_into('<I',fat,c*4,0)
with IMG.open('r+b') as f:
    for copy in range(2):f.seek((v.reserved+copy*v.fatsz)*512);f.write(fat)
    f.seek(entry[0]*512+entry[1]+28);f.write(struct.pack('<I',len(keep)*512))
    fsi=wal.U16(v.bpb,48);backup=wal.U16(v.bpb,50)
    for sec in (fsi,backup+fsi):
        f.seek(sec*512+488);f.write(struct.pack('<II',len(release),release[0]))
run('/sbin/fsck.fat','-n',IMG);refused();filler.unlink()
print('PASS fragmented allocation rejected on staged copy; no unready file/metadata left on original')
# Offset preparation is allowed only on private images; GPT/MSR/backup untouched.
fresh();raw=W/'raw.img';shutil.copyfile(IMG,raw)
sys.path.insert(0,str(ROOT/'tests/boot'));from mkusb import make_gpt
make_gpt(raw,IMG);first=34816
with IMG.open('rb') as f:prefix=f.read(first*512);f.seek((first+TOTAL)*512);suffix=f.read()
run(sys.executable,TOOL,IMG,'--prepare','--confirm','12345678','--start',first)
with IMG.open('rb') as f:assert f.read(first*512)==prefix;f.seek((first+TOTAL)*512);assert f.read()==suffix
print('PASS complete offset preparation; GPT primary/backup/MSR/gaps unchanged')
# Simulated sysfs/proc device graph. Test the SAME policy function, not a model
# of its result. No block nodes are created/opened, O_EXCL/ioctl are not faked as
# evidence of hardware qualification.
S=W/'fake-linux';shutil.rmtree(S,ignore_errors=True)
links=S/'sys/dev/block';links.mkdir(parents=True)
disk=S/'sys/devices/disk';disk.mkdir(parents=True);(disk/'dev').write_text('8:0')
for n in (1,2):
    p=disk/f'part{n}';p.mkdir();(p/'partition').write_text(str(n));(p/'dev').write_text(f'8:{n}')
dm=S/'sys/devices/dm';(dm/'slaves').mkdir(parents=True);(dm/'dev').write_text('253:0')
(dm/'slaves/part1').symlink_to((disk/'part1').resolve())
for mm,p in [('8:0',disk),('8:1',disk/'part1'),('8:2',disk/'part2'),('253:0',dm)]:
    (links/mm).symlink_to(p.resolve())
proc=S/'proc/self';proc.mkdir(parents=True);mountinfo=proc/'mountinfo';swaps=S/'proc/swaps'
mountinfo.write_text('');swaps.write_text('Filename Type Size Used Priority\n')
RealPath=Path

def fakepath(path):
    p=str(path)
    if p.startswith('/sys/dev/block') or p in ('/proc/self/mountinfo','/proc/swaps'):return S/p.lstrip('/')
    return RealPath(path)

def gate(major=8,minor=1):wal.unmounted_partition(SimpleNamespace(st_rdev=os.makedev(major,minor)))
def failgate():
    try:gate()
    except ValueError:return
    raise AssertionError('mounted/dependent target accepted')
with patch.object(wal,'Path',fakepath):
    gate()
    try:gate(minor=0)
    except ValueError:pass
    else:raise AssertionError('whole disk accepted')
    mountinfo.write_text('1 0 8:1 / /mnt rw - vfat /dev/fake rw\n');failgate()
    mountinfo.write_text('1 0 8:0 / /mnt rw - vfat /dev/fake rw\n');failgate()
    mountinfo.write_text('1 0 253:0 / /mnt rw - ext4 /dev/dm rw\n');failgate()
    mountinfo.write_text('1 0 8:2 / /mnt rw - vfat /dev/other-partition rw\n');gate()
    mountinfo.write_text('');swaps.write_text('Filename Type Size Used Priority\n/dev/fake-swap partition 1 0 0\n')
    realstat=os.stat
    def fakestat(path,*a,**kw):
        if str(path)=='/dev/fake-swap':return SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=os.makedev(8,1))
        return realstat(path,*a,**kw)
    with patch.object(wal.os,'stat',fakestat):failgate()
print('PASS SIMULATED Linux gates: whole disk, mounted target/parent, mapper dependency, swap; unmounted partition accepted')
print('PASS provisioner software policy checks; real O_EXCL/ioctl/physical USB remain UNTESTED')
# Real FAT COW log exhaustion: leave ONE free cluster per FAT sector. A 64 KiB
# file then needs >48 distinct metadata sectors, despite ample free bytes.
fresh();text.write_bytes(b'O'*1000)
run('mcopy','-i',IMG,text,'::Old long name.txt')
run(sys.executable,TOOL,IMG,'--prepare','--confirm','12345678')
fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL);free=wal.U32(v.read(wal.U16(v.bpb,48)),488);os.close(fd)
with filler.open('wb') as f:f.truncate(free*512)
run('mcopy','-i',IMG,filler,'::FILL.BIN')
fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL)
entry=next(r for r in v.entries(v.root) if r[2][:11]==b'FILL    BIN')
c=(wal.U16(entry[2],20)<<16)|wal.U16(entry[2],26);chain=list(v.chain(c));release=chain[::128];freed=set(release)
keep=[c for c in chain if c not in freed];fat=bytearray(v.read(v.reserved,v.fatsz));os.close(fd)
for i,c in enumerate(keep):struct.pack_into('<I',fat,c*4,keep[i+1] if i+1<len(keep) else 0x0FFFFFFF)
for c in release:struct.pack_into('<I',fat,c*4,0)
d=bytearray(entry[2]);struct.pack_into('<H',d,20,keep[0]>>16);struct.pack_into('<H',d,26,keep[0]&65535)
struct.pack_into('<I',d,28,len(keep)*512)
with IMG.open('r+b') as f:
    for copy in range(2):f.seek((v.reserved+copy*v.fatsz)*512);f.write(fat)
    f.seek(entry[0]*512+entry[1]);f.write(d)
    fsi=wal.U16(v.bpb,48);backup=wal.U16(v.bpb,50)
    for sec in (fsi,backup+fsi):f.seek(sec*512+488);f.write(struct.pack('<II',len(release),release[0]))
run('/sbin/fsck.fat','-n',IMG)
# Save FAT+FSInfo+root separately; fresh unreferenced COW data/WAL are allowed
# to change, but all reachable metadata and old bodies must be restored.
with IMG.open('rb') as f:before=f.read(v.data*512);root_before=os.pread(f.fileno(),512,v.sector(v.root)*512)
exe=Path(os.environ.get('FAT_OWNED_HARNESS',str(ROOT/'tests/fs/build/fat-owned-sanitize')))
p=run(exe,IMG,0,TOTAL,'large');assert 'result=-' in p.stdout,p.stdout
run(exe,IMG,0,TOTAL,'mount');run('/sbin/fsck.fat','-n',IMG)
with IMG.open('rb') as f:assert f.read(v.data*512)==before;assert os.pread(f.fileno(),512,v.sector(v.root)*512)==root_before
out=W/'old-export';run('mcopy','-i',IMG,'::Old long name.txt',out);assert out.read_bytes()==b'O'*1000;out.unlink()
filler.unlink();run(sys.executable,TOOL,IMG)
print('PASS real fragmented FAT exceeds 48 undo records: no false success, exact metadata rollback, old body + clean fsck')
# The BPB root is a reference with NO parent dirent. A forged self-owned root
# used to escape a dirent-only crosslink scan. Construct it on a disposable
# file, NEVER enable/write it, and prove both kernel enrollment + tool refuse.
from mkfatowned import make as make_owned
p,first,total,j,bpb=make_owned(W/'root-crosslink','mbr');raw=W/'root-crosslink/fat-part.img'
shutil.copyfile(raw,IMG);fd=os.open(IMG,os.O_RDONLY);v=wal.Volume(fd,0,TOTAL);sec,fields,row=v.owner();os.close(fd)
b=bytearray(bpb);root_cluster=fields[5]+2;struct.pack_into('<I',b,44,root_cluster)
fields=list(fields);fields[1]=wal.zlib.crc32(b);fields[8]=sec+2;fields[9]=0;fields[12]=root_cluster
h=bytearray(512);h[:16]=wal.MAGIC;struct.pack_into('<16I',h,64,*fields);struct.pack_into('<I',h,508,wal.zlib.crc32(h[:508]))
with IMG.open('r+b') as f:
    f.write(b);f.seek(wal.U16(b,50)*512);f.write(b)
    f.seek(sec*512);f.write(h);f.seek((sec+2)*512);f.write(row[2]+bytes(480))
before=digest();result=run(exe,IMG,0,TOTAL,'mount',code=4);assert 'writes=0' in result.stdout
run(sys.executable,TOOL,IMG,code=1);assert digest()==before
print('PASS BPB root overlaps journal chain (CRC-valid forged owner): kernel + tool refuse BEFORE any write')
