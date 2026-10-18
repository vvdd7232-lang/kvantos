#!/usr/bin/env python3
"""Real AHCI DMA -> block -> GPT/MBR/FAT/VFS/GUI on disposable SATA images.
Both BIOS/i386 and UEFI/x86-64. No physical device or disk write qualification.
"""
from pathlib import Path
import ctypes as C
import hashlib,importlib.util,os,re,shutil,struct,subprocess,sys,time,zlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
from mkusb import make,make_gpt
from text_model import editor_type
Editor=editor_type(smoke.IS64)
smoke.WORK=ROOT/'build/ahci-smoke'/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'legacy.img'
subprocess.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=subprocess.DEVNULL)
raw,mbr,proof=make(smoke.WORK)
# Place the fault target well beyond firmware/grub partition-probe read-ahead.
tmp=smoke.WORK/'temp.bin';tmp.write_bytes(b'\0'*(2*1024*1024))
subprocess.run(['mcopy','-i',str(raw),str(tmp),'::PADDING.BIN'],check=True)
fault_body=b'AHCI read-error proof\n'*100;tmp.write_bytes(fault_body)
subprocess.run(['mcopy','-i',str(raw),str(tmp),'::FAULT.TXT'],check=True)
# A clean owner may opt in only explicitly; this read regression disables it again.
with raw.open('rb') as f:serial=struct.unpack_from('<I',f.read(512),67)[0]
subprocess.run([sys.executable,'tools/fat-wal.py',str(raw),'--prepare','--confirm',f'{serial:08X}'],check=True,capture_output=True)
with mbr.open('r+b') as dst,raw.open('rb') as src:
    dst.seek(2048*512)
    while b:=src.read(1024*1024):dst.write(b)
gpt=smoke.WORK/'sata-gpt.img';make_gpt(raw,gpt)
# Bypass all host writes while the guest is running. Check whole images later.
def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        while b:=f.read(1024*1024):h.update(b)
    return h.hexdigest()
original={p:digest(p) for p in (raw,mbr,gpt)}
def expected(name):
    return subprocess.check_output(['mtype','-i',str(raw),'::'+name])
large=expected('MULTI.C');hello=expected('HELLO.C')

def sata_args(image,config=None,second=True):
    filename=f'blkdebug:{config}:{image}' if config else str(image)
    out=['-device','ich9-ahci,id=satatest','-drive',f'if=none,id=satafile,format=raw,file={filename}',
         '-device','ide-hd,id=satadisk,bus=satatest.2,drive=satafile,model=KVANT-SATA-MAIN']
    if second:out+=['-drive',f'if=none,id=sataraw,format=raw,file={raw}',
                   '-device','ide-hd,bus=satatest.5,drive=sataraw,model=KVANT-SATA-RAW']
    return out

def command(q,line):
    at=len(q.log.read_bytes());q.text(line+'\n');deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        out=q.log.read_bytes()[at:]
        if b'kvant:/$ ' in out:return out
        time.sleep(.03)
    raise AssertionError(('shell command timed out',line,q.log.read_bytes()[-1500:]))

def state(q,app):
    return Editor.from_buffer_copy(q.mem('dev_editor' if app==9 else 'note_editor',C.sizeof(Editor)))
def choose(q,name,app):
    s=state(q,app);names=[bytes(e.name).decode() for e in s.items[:s.count]]
    assert name in names,(name,names)
    wid=2000+names.index(name)
    for _ in range(128):
        if wid in {h[4] for h in q.hits(app)}:break
        q.widget(86,app)
    else:raise AssertionError(('row not visible',name))
    q.widget(wid,app)
def save_log(q,name):
    (smoke.WORK/(name+'.serial.log')).write_bytes(q.log.read_bytes())

print('AHCI normal guest:', 'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
for image,part,label in ((mbr,'sata0p1','mbr'),(gpt,'sata0p2','gpt')):
    root='/mnt/'+part
    with smoke.Guest(disk=True,extra_args=sata_args(image)) as q:
        assert q.num('ahci_drive_count')==2
        out=command(q,'disk');assert b'KVANT-SATA-MAIN' in out and b'DMA, default RO' in out,out
        out=command(q,'mount');assert root.encode() in out and b'/mnt/sata1 ' in out,out
        assert all(b' ro' in line for line in out.splitlines() if b'/mnt/sata' in line),out
        assert b'RW enabled for this session' in command(q,'fatwrite '+part+' enable')
        assert b'FAT32 RO' in command(q,'fatwrite '+part+' disable')
        out=command(q,f'vcp {root}/MULTI.C /mnt/ram/ahci.c');assert b'Copied' in out,out
        assert q.files()['ahci.c']==large
        out=command(q,'vcp /mnt/sata1/HELLO.C /mnt/kv/sata.c');assert b'Copied' in out,out
        # Independent raw SATA device and legacy IDE/KvFS coexist in the same VM.
        out=command(q,'vcat /mnt/kv/sata.c');assert b'USB import OK' in out,out
        out=command(q,f'vcp /mnt/ram/ahci.c {root}/SHOULD.C');assert b'Copied' not in out,out
        for _ in range(2):command(q,'rescan')
        out=command(q,'mount');assert root.encode() in out and b'/mnt/sata1 ' in out,out
        if label=='gpt':
            q.text('guimenu\n');time.sleep(.5)
            for app in (7,9):
                q.open_app(app);q.widget(96,app)
                if q.mode(app)==3:q.widget(84,app)
                assert q.mode(app)==6
                assert bytes(state(q,app).path).decode()==root
                choose(q,'Sources',app);choose(q,'A long source name.C',app)
                q.choose_name('sata-note.txt' if app==7 else 'sata-code.c');q.widget(82,app)
                assert q.mode(app)==0 and q.document(app)['text']==proof
            q.screenshot(smoke.WORK/'sata-gui.png')
        save_log(q,label+'-basic')
    assert digest(image)==original[image] and digest(raw)==original[raw]
    print(f'  {label}: two SATA ports + raw/partitioned FAT, owned RO, DMA exact bytes, IDE/KvFS, rescan, GUI if GPT PASS',flush=True)
    # Full power cycle, no in-memory FAT/GRUB/DMA state survives.
    with smoke.Guest(disk=True,extra_args=sata_args(image)) as q:
        out=command(q,f'vcat {root}/HELLO.C');assert b'USB import OK' in out,out
        out=command(q,'vcat /mnt/kv/sata.c');assert b'USB import OK' in out,out
        save_log(q,label+'-cold')
    assert digest(image)==original[image]
    print(f'  {label}: cold SATA read + persisted IDE copy, whole source unchanged PASS',flush=True)

# A valid pending begin with zero undo records requires a real closing flush.
# M3.2 must recover it before publication, then expose the namespace as RO.
pending=smoke.WORK/'pending.img';shutil.copyfile(raw,pending)
spec=importlib.util.spec_from_file_location('ahci_wal_tool',ROOT/'tools/fat-wal.py')
wal=importlib.util.module_from_spec(spec);spec.loader.exec_module(wal)
fd=os.open(raw,os.O_RDONLY)
try:owner=wal.Volume(fd,0,raw.stat().st_size//512).owner()[0]
finally:os.close(fd)
with pending.open('r+b') as f:
    f.seek((owner+1)*512);header=bytearray(f.read(512))
    assert header[:8]==b'KVSTX01\0'
    struct.pack_into('<II',header,32,1,0)
    struct.pack_into('<I',header,508,zlib.crc32(header[:508]))
    f.seek((owner+1)*512);f.write(header)
pending_before=digest(pending)
with smoke.Guest(disk=False,extra_args=sata_args(pending,second=False)) as q:
    out=command(q,'mount');assert b'/mnt/sata0' in out and b' ro' in out,out
    assert b'FAT owned WAL recovered BEFORE namespace publication' in q.log.read_bytes()
    save_log(q,'pending-recovered')
assert digest(pending)!=pending_before
with pending.open('rb') as f:
    f.seek((owner+1)*512);closed=f.read(512)
assert struct.unpack_from('<II',closed,32)==(0,0)
subprocess.run(['/sbin/fsck.fat','-n',str(pending)],check=True,capture_output=True)
print('  valid pending WAL -> recovery BEFORE namespace, default RO + external fsck PASS',flush=True)

# Real disk read EIO -> no false copy/partial caller buffer; port fences.
with raw.open('rb') as f:b=f.read(512)
spc=b[13];reserved=struct.unpack_from('<H',b,14)[0];fats=b[16];fatsz=struct.unpack_from('<I',b,36)[0]
chain=subprocess.check_output(['mshowfat','-i',str(raw),'::FAULT.TXT'],text=True)
cluster=int(re.search(r'<(\d+)',chain).group(1))
sector=2048+reserved+fats*fatsz+(cluster-2)*spc
config=smoke.WORK/'read-error.conf'
config.write_text(f'[inject-error]\nevent = "read_aio"\nsector = "{sector}"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n')
with smoke.Guest(disk=False,extra_args=sata_args(mbr,config,second=False)) as q:
    out=command(q,'vcp /mnt/sata0p1/FAULT.TXT /mnt/ram/fault.txt')
    assert b'Copied' not in out and 'fault.txt' not in q.files(),out
    out=command(q,'disk');assert b'fenced; reboot required' in out,out
    out=command(q,'rescan');assert b'kvant:/$' in out,out
    assert b'/mnt/sata0p1' not in command(q,'mount')
    save_log(q,'read-error')
assert digest(mbr)==original[mbr]
with smoke.Guest(disk=False,extra_args=sata_args(mbr,second=False)) as q:
    out=command(q,'vcp /mnt/sata0p1/FAULT.TXT /mnt/ram/retry.txt')
    assert b'Copied' in out and q.files()['retry.txt']==fault_body,out
    save_log(q,'after-error-cold')
print('  actual AHCI DMA read EIO -> fenced port, bounded shell/rescan, fresh cold read PASS',flush=True)
# Independent external FAT check and whole-source hash checks (GPT/gaps included).
subprocess.run(['/sbin/fsck.fat','-n',str(raw)],check=True,capture_output=True)
assert {p:digest(p) for p in original}==original
print('PASS all AHCI software read-only guest checks; not physical SATA qualification',flush=True)
