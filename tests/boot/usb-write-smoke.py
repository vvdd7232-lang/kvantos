#!/usr/bin/env python3
"""M2 transport lab: WRITE(10)/SYNC only in a marked post-partition scratch gap.
Requires make STORAGE_LAB=1 iso; never attach real media. FAT is NOT write-enabled.
"""
from pathlib import Path
import os,sys,subprocess,struct,time,hashlib,shutil
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
from mkusb import make,make_gpt
CONTROLLER=os.environ.get('USB_CONTROLLER','ehci-ohci')
assert CONTROLLER in ('uhci','ehci-ohci')
LAYOUT=os.environ.get('USB_LAYOUT','gpt');assert LAYOUT in ('gpt','mbr')
smoke.WORK=ROOT/'build/usb-write'/CONTROLLER/LAYOUT/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
assert 'storage_lab_stage' in smoke.symbols,'build a STORAGE_LAB=1 image first'
raw,mbr,_=make(smoke.WORK)
if LAYOUT=='gpt':source=make_gpt(raw,smoke.WORK/'lab-gpt.img')
else:
    source=mbr
    with source.open('ab') as f:f.truncate(source.stat().st_size+2048*512)
sectors=source.stat().st_size//512;marker=sectors-1024;start=marker+1
if LAYOUT=='gpt':fat_start=34816
else:fat_start=2048
fat_bytes=raw.stat().st_size
assert start>fat_start+fat_bytes//512 and start+32<sectors-33
magic=b'KVANTOS-M2-DISPOSABLE-ONLY\0';header=bytearray(512);header[:len(magic)]=magic
struct.pack_into('<II',header,64,sectors,1)
with source.open('r+b') as f:f.seek(marker*512);f.write(header)
original=source.read_bytes()
pattern=bytes((i*29+(i>>8)+0x53)&255 for i in range(17*512))
def args(path,readonly=False,config=None):
    filename=f'blkdebug:{config}:{path}' if config else str(path)
    a=['-drive',f'if=none,id=flash,format=raw,file={filename},readonly={"on" if readonly else "off"}']
    if CONTROLLER=='uhci':a+=['-device','piix3-usb-uhci,id=uhci','-device','usb-storage,id=flashdev,bus=uhci.0,drive=flash,port=1']
    else:a+=['-device','ich9-usb-ehci1,id=ehci','-device','pci-ohci,id=ohci1,masterbus=ehci.0,firstport=0','-device','pci-ohci,id=ohci2,masterbus=ehci.0,firstport=3','-device','usb-storage,id=flashdev,bus=ehci.0,drive=flash,port=3']
    return a
counter=0
def run(mode='roundtrip',readonly=False,config=None,detach=False):
    global counter
    counter+=1
    q=smoke.Guest(False,extra_args=args(source,readonly,config))
    with q:
        assert q.num('disk_count')==1,q.log.read_text(errors='replace')
        q.text('usbtest '+mode)
        q.cmd('sendkey ret 40')
        deadline=time.monotonic()+15
        if detach:
            while time.monotonic()<deadline and q.num('storage_lab_stage')!=2:time.sleep(.005)
            assert q.num('storage_lab_stage')==2,'did not reach armed I/O phase'
            q.cmd('device_del flashdev')
        while time.monotonic()<deadline and q.num('storage_lab_stage')!=3:time.sleep(.02)
        assert q.num('storage_lab_stage')==3,q.log.read_text(errors='replace')[-600:]
        result=struct.unpack('<i',q.mem('storage_lab_result',4))[0]
        text=q.log.read_text(errors='replace')
        (smoke.WORK/f'case-{counter}.log').write_text(text)
        return result

def outside_unchanged():
    now=source.read_bytes();lo=start*512;hi=(start+32)*512
    assert now[:lo]==original[:lo] and now[hi:]==original[hi:],'write escaped authorized scratch interval'
    # Independent fsck of the actual FAT partition, not the raw scratch extent.
    part=smoke.WORK/'fsck-partition.img'
    with source.open('rb') as f:f.seek(fat_start*512);part.write_bytes(f.read(fat_bytes))
    assert hashlib.sha256(part.read_bytes()).digest()==hashlib.sha256(raw.read_bytes()).digest()
    p=subprocess.run(['/usr/sbin/fsck.fat','-n',str(part)],capture_output=True,text=True)
    assert p.returncode==0,p.stdout+p.stderr
    part.unlink()
print('USB write transport lab:',CONTROLLER,LAYOUT,'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
assert run()==0
now=source.read_bytes();assert now[start*512:start*512+len(pattern)]==pattern
outside_unchanged();assert run('verify')==0
print('  17-sector WRITE(10), chunking, SYNCHRONIZE CACHE, stale-cache invalidation, reboot verify, unchanged GPT/FAT + fsck PASS',flush=True)
# The exact same build must refuse media lacking the independent marker.
with source.open('r+b') as f:f.seek(marker*512);f.write(b'\0'*512)
before=hashlib.sha256(source.read_bytes()).digest();assert run()==-12
assert hashlib.sha256(source.read_bytes()).digest()==before
with source.open('r+b') as f:f.seek(marker*512);f.write(header)
before=hashlib.sha256(source.read_bytes()).digest()
assert run(readonly=True)==-1;assert hashlib.sha256(source.read_bytes()).digest()==before
outside_unchanged()
print('  Marker guard, range lock, default lock, read-only SCSI failure and revoked grant PASS',flush=True)
# Inject backend EIO separately in write and flush paths; successful DATA OUT alone is not success.
for event in ('write_aio','flush_to_disk'):
    config=smoke.WORK/(event+'.conf')
    config.write_text(f'[inject-error]\nevent = "{event}"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n')
    assert run(config=config)==-1,(event,'failure was reported as success')
    outside_unchanged()
print('  Backend write EIO and flush EIO: failure, no replay, revoked grant, bounded return PASS',flush=True)
assert run('detach',detach=True)==-1;outside_unchanged()
assert run('verify')==0
print('  Unplug during armed write loop, bounded error, reconnect/reboot readback + fsck PASS',flush=True)
