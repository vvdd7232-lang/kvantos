#!/usr/bin/env python3
"""NORMAL ISO, real USB or AHCI/FAT/VFS and owned file WAL. Private images only.
Debugger stops are outside the kernel: no product test hooks or RAM patching.
Cold power loss, transport EIO/RO, failed recovery, unplug/reconnect, full disk,
independent fsck + complete namespace + all outside-partition bytes checked.
"""
from pathlib import Path
import hashlib,os,shutil,struct,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
import usb_topology as topology
from mkfatowned import make
from qemu_break import Breakpoint
assert 'stx_lab_pause_at' not in smoke.symbols and 'fat_test_journal' not in smoke.symbols,'normal ISO required'
CONTROLLER=os.environ.get('STORAGE_CONTROLLER',os.environ.get('USB_CONTROLLER','ehci-ohci'));LAYOUT=os.environ.get('STORAGE_LAYOUT',os.environ.get('USB_LAYOUT','gpt'))
assert CONTROLLER in ('uhci','ohci','ehci-ohci','ahci') and LAYOUT in ('mbr','gpt')
PHASE=os.environ.get('OWNED_PHASE','all')
assert PHASE in ('all','main','full','smoke')
smoke.WORK=ROOT/'build/fat-owned-guest'/CONTROLLER/LAYOUT/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK=topology.work(smoke.WORK)
if PHASE!='all':smoke.WORK=smoke.WORK/('phase-'+PHASE)
smoke.WORK.mkdir(parents=True,exist_ok=True)
source,first,total,journal,bpb=make(smoke.WORK,LAYOUT)
vol=('sata0p' if CONTROLLER=='ahci' else 'usb0p')+('2' if LAYOUT=='gpt' else '1');root='/mnt/'+vol
body=bytes((i*19+7)&255 for i in range(1300));local=smoke.WORK/'source.bin';local.write_bytes(body)
loc=str(source)+'@@'+str(first*512)
subprocess.run(['mcopy','-i',loc,str(local),'::SOURCE.BIN'],check=True,capture_output=True)
size=source.stat().st_size;chunks=[]
with source.open('rb') as f:
    while b:=f.read(65536):
        if b.strip(b'\0'):chunks.append((f.tell()-len(b),b))
def reset():
    with source.open('wb') as f:
        f.truncate(size)
        for at,b in chunks:f.seek(at);f.write(b)
def span(a,b):
    h=hashlib.sha256()
    with source.open('rb') as f:
        f.seek(a)
        while a<b:
            data=f.read(min(65536,b-a));assert data;h.update(data);a+=len(data)
    return h.digest()
ranges=[(0,first*512),((first+total)*512,size),((journal-1)*512,journal*512)]
outside=[span(a,b) for a,b in ranges]
def external():
    assert [span(a,b) for a,b in ranges]==outside,'writes escaped FAT / changed immutable owner'
    part=smoke.WORK/'check.img'
    with source.open('rb') as src,part.open('wb') as dst:
        dst.truncate(total*512);src.seek(first*512)
        for at in range(0,total*512,65536):
            b=src.read(min(65536,total*512-at))
            if b.strip(b'\0'):dst.seek(at);dst.write(b)
    p=subprocess.run(['/sbin/fsck.fat','-n',str(part)],capture_output=True,text=True)
    assert p.returncode==0,(p.stdout,p.stderr)
    out=smoke.WORK/'export'
    if out.exists():shutil.rmtree(out)
    out.mkdir();subprocess.run(['mcopy','-s','-i',str(part),'::*',str(out)],check=True,capture_output=True)
    return {p.relative_to(out).as_posix():(None if p.is_dir() else hashlib.sha256(p.read_bytes()).digest())
            for p in out.rglob('*') if p.name.upper()!='KVANTWAL.SYS'}
old=external();counter=0

def device():
    if CONTROLLER=='ahci':return 'ide-hd,id=stick,bus=satatest.2,drive=flash'
    return 'usb-storage,id=stick,bus=usbtest.0,drive=flash,port='+topology.leaf_port('1' if CONTROLLER in ('uhci','ohci') else '3')
def args(config=None,readonly=False):
    (smoke.WORK/'debug.sock').unlink(missing_ok=True)
    if readonly and CONTROLLER=='ahci':
        # QEMU IDE rejects readonly=on at device construction. Model a permanently
        # write-denied backend explicitly; do not call this physical write protect.
        config=smoke.WORK/'write-denied.conf'
        config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "13"\nonce = "off"\nimmediately = "on"\n')
    filename=f'blkdebug:{config}:{source}' if config else str(source)
    a=['-gdb','unix:'+str(smoke.WORK/'debug.sock')+',server=on,wait=off','-drive',
       f'if=none,id=flash,format=raw,file={filename},readonly={"on" if readonly and CONTROLLER!="ahci" else "off"}']
    if CONTROLLER=='ahci':a+=['-device','ich9-ahci,id=satatest']
    elif CONTROLLER=='ohci':a+=['-device','pci-ohci,id=usbtest']
    elif CONTROLLER=='uhci':a+=['-device','piix3-usb-uhci,id=usbtest']
    else:a+=['-device','ich9-usb-ehci1,id=usbtest','-device','pci-ohci,id=comp0,masterbus=usbtest.0,firstport=0',
             '-device','pci-ohci,id=comp1,masterbus=usbtest.0,firstport=3']
    return a+topology.hub_args(CONTROLLER)+['-device',device()]
def guest(**kw):return smoke.Guest(disk=False,extra_args=args(**kw))
def command(q,line,timeout=60):
    at=len(q.log.read_bytes());q.text(line+'\n');deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        out=q.log.read_bytes()[at:]
        if b'kvant:/$' in out:return out
        time.sleep(.03)
    raise AssertionError(('shell command timeout',line,q.log.read_bytes()[-1000:]))
def enable(q):assert b'RW enabled for this session' in command(q,'fatwrite '+vol+' enable')
def copy(q,name='NEW.TXT',timeout=60):return command(q,f'vcp {root}/SOURCE.BIN {root}/{name}',timeout)
def log(q,label):
    global counter
    counter+=1;(smoke.WORK/f'case-{counter:03}-{label}.log').write_bytes(q.log.read_bytes())
def cold(expected=old):
    with guest() as q:
        out=command(q,'mount');assert (root.encode() in out and b' ro' in out),out
        assert b'RW enabled for this session' not in q.log.read_bytes()
        log(q,'cold')
    assert external()==expected

def pending(symbol='stx_commit'):
    with guest() as q:
        enable(q);bp=Breakpoint(q,smoke.WORK/'debug.sock',symbol)
        before=q.log.read_bytes();q.text(f'vcp {root}/SOURCE.BIN {root}/NEW.TXT\n');bp.wait()
        assert b'Copied ' not in q.log.read_bytes()[len(before):],'interrupted operation reported success'
        log(q,'crash-'+symbol);q.proc.kill();q.proc.wait(timeout=5);bp.close()

print('Owned WAL scope:',PHASE,flush=True)
if PHASE!='full':
    print('Normal owned WAL:',CONTROLLER,LAYOUT,'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
    reset();before=span(0,size)
    with guest() as q:
        assert b'read-only' in copy(q);log(q,'default-ro')
    assert span(0,size)==before
    with guest() as q:
        enable(q);assert b'Copied 1300 bytes' in copy(q)
        assert b'Created:' in command(q,'vmkdir '+root+'/DIR')
        assert b'Copied 1300 bytes' in copy(q,'DIR/CHILD.TXT')
        assert b'Deleted:' in command(q,'vrm '+root+'/DIR/CHILD.TXT')
        assert b'Deleted:' in command(q,'vrm '+root+'/DIR')
        assert b'FAT32 RO' in command(q,'fatwrite '+vol+' disable')
        assert b'read-only' in copy(q,'NO.TXT');log(q,'success-disable')
    new=dict(old);new['NEW.TXT']=hashlib.sha256(body).digest();cold(new)
    print('  default RO -> explicit enable -> write/mkdir/remove/disable -> cold RO/fsck PASS',flush=True)
    for symbol in (('stx_commit',) if PHASE=='smoke' else ('stx_write','write_entry','stx_commit')):
        reset();pending(symbol);cold()
        print('  actual power loss at',symbol,'-> recovery BEFORE namespace + old namespace/fsck PASS',flush=True)
    reserved=struct.unpack_from('<H',bpb,14)[0];fatsz=struct.unpack_from('<I',bpb,36)[0]
    root_sector=first+reserved+2*fatsz
    for label,event,sector in [('FAT2-EIO','write_aio',first+reserved+fatsz),('directory-EIO','write_aio',root_sector),
                               ('WAL-EIO','write_aio',journal),('flush-EIO','flush_to_disk',None)]:
        if PHASE=='smoke' and label!='FAT2-EIO':continue
        reset();config=smoke.WORK/(label+'.conf')
        config.write_text(f'[inject-error]\nevent = "{event}"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n'+
                          (f'sector = "{sector}"\n' if sector is not None else ''))
        with guest(config=config) as q:
            enable(q);out=copy(q);assert b'Copied ' not in out and b'Write error' in out,out
            second=command(q,f'vcp /mnt/ram/readme.txt {root}/SECOND.TXT')
            assert b'read-only' in second,second;log(q,label)
        cold();print(' ',label,'-> no false success, lease revoked, cold recovery/fsck PASS',flush=True)
    reset();before=span(0,size)
    with guest(readonly=True) as q:
        enable(q);out=copy(q);assert b'Copied ' not in out and b'Write error' in out,out;log(q,'write-denied' if CONTROLLER=='ahci' else 'hardware-RO')
    assert span(0,size)==before
    cold();print('  '+('permanent write-denied EACCES backend' if CONTROLLER=='ahci' else 'read-only SCSI backend')+' -> error/fence, whole image unchanged PASS',flush=True)
    for label,event,sector in [('recovery-write','write_aio',first+reserved+fatsz),('recovery-read','read_aio',journal+1)]:
        if PHASE=='smoke':continue
        reset();pending();before=span(0,size);config=smoke.WORK/(label+'.conf')
        config.write_text(f'[inject-error]\nevent = "{event}"\nsector = "{sector}"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n')
        with guest(config=config) as q:
            assert root.encode() not in command(q,'mount');log(q,label)
        if event=='read_aio':assert span(0,size)==before
        cold();print(' ',label,'failure -> namespace withheld, next cold replay/fsck PASS',flush=True)
    for at,label in [((journal-1)*512+100,'owner'),(journal*512+100,'journal')]:
        if PHASE=='smoke':continue
        reset()
        with source.open('r+b') as f:f.seek(at);f.write(b'X')
        before=span(0,size)
        with guest() as q:assert root.encode() not in command(q,'mount');log(q,'invalid-'+label)
        assert span(0,size)==before
    if PHASE!='smoke':print('  bad owner/header -> no namespace, WHOLE image unchanged PASS',flush=True)
    if CONTROLLER=='ahci':
        print('  AHCI hotplug not emulated: link loss covered by host model, recovery by full power cycle (NOT a physical unplug claim)',flush=True)
    else:
        reset()
        with guest() as q:
            enable(q);assert b'Error' not in q.cmd('device_del stick');command(q,'rescan')
            assert q.num('disk_count')==0
            reply=q.cmd(f'drive_add 0 if=none,id=flash,file={source},format=raw');assert b'Error:' not in reply,reply
            reply=q.cmd('device_add '+device());assert b'Error:' not in reply,reply
            command(q,'rescan');assert b'read-only' in copy(q)
            enable(q);assert b'Copied 1300 bytes' in copy(q);log(q,'reconnect-fresh-enable')
        cold(new);print('  actual unplug/reconnect/rescan -> fresh RO namespace, new explicit opt-in PASS',flush=True)
if PHASE in ('all','full'):
    reset()
    # Reserve the directory slot BEFORE consuming all free clusters. Otherwise
    # mtools may allocate the entire body, fail to grow the full root directory,
    # and leave a lost chain even with exit status 0. Independent fsck catches it.
    empty=smoke.WORK/'empty.bin';empty.write_bytes(b'')
    subprocess.run(['mcopy','-i',loc,str(empty),'::FILL.BIN'],check=True,capture_output=True)
    with source.open('rb') as f:f.seek((first+struct.unpack_from('<H',bpb,48)[0])*512+488);free=struct.unpack('<I',f.read(4))[0]
    filler=smoke.WORK/'fill.bin'
    with filler.open('wb') as f:f.truncate(free*512)
    subprocess.run(['mcopy','-o','-i',loc,str(filler),'::FILL.BIN'],check=True,capture_output=True)
    full=external()
    with guest() as q:
        enable(q);out=copy(q,timeout=60*(topology.DEPTH+1));assert b'Not enough space' in out and b'Copied ' not in out,out;log(q,'full')
    cold(full);filler.unlink()
    print('  full filesystem -> old namespace/data intact, cold fsck PASS',flush=True)
