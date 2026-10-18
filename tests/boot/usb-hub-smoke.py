#!/usr/bin/env python3
"""FS/HS tree / explicit TD-level async-TT LAB (not wire qualification): two hubs + three distinct disks, global addresses, cached
reads after leaf/ancestor unplug, sticky refusal, reconnect, repeated rescans.
Normal production ISO; all media are private regular-file FAT32 fixtures.
"""
from pathlib import Path
import ctypes as C
import hashlib,os,struct,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
from mkusb import make
from usb_topology import hub_model,hub_device,TT_LAB
CONTROLLER=os.environ.get('USB_CONTROLLER','ohci')
assert CONTROLLER in ('uhci','ohci','ehci')
MODEL=hub_model(CONTROLLER)
smoke.WORK=ROOT/'build/usb-hubs'/CONTROLLER/('uefi64' if smoke.IS64 else 'bios32')
if TT_LAB:smoke.WORK=smoke.WORK/'async-tt-lab'
smoke.WORK.mkdir(parents=True,exist_ok=True)
assert 'fat_lab_stage' not in smoke.symbols
pointer=C.c_uint64 if smoke.IS64 else C.c_uint32
class Disk(C.Structure):
    _fields_=[('hc',pointer),('sectors',C.c_uint32),('write_start',C.c_uint32),('write_count',C.c_uint32),('generation',C.c_uint32),
              *[(n,C.c_uint8) for n in ('write_allowed','sense_key','asc','ascq')],('in_mps',C.c_uint16),('out_mps',C.c_uint16),
              *[(n,C.c_uint8) for n in ('address','port','ep0','in_ep','out_ep','itoggle','otoggle','iface','parent','downport','route_dead','speed')]]
class HC(C.Structure):
    _fields_=[('op',pointer),('io',C.c_uint16),*[(n,C.c_uint8) for n in ('kind','ports','dead','bus','slot','func','pool')],('busy',C.c_uint32)]
class Hub(C.Structure):
    _fields_=[("dev",Disk),("blocked",C.c_uint16),("ports",C.c_uint8)]
def disks(q):
    count=q.num('disk_count');b=q.mem('disks',C.sizeof(Disk)*count)
    return [Disk.from_buffer_copy(b,i*C.sizeof(Disk)) for i in range(count)]
def command(q,line):
    at=len(q.log.read_bytes());q.text(line+'\n');end=time.monotonic()+60
    while time.monotonic()<end:
        out=q.log.read_bytes()[at:]
        if b'kvant:/$ ' in out:return out
        time.sleep(.03)
    raise AssertionError(('hub command timeout',line))
def monitor(q,line):
    out=q.cmd(line);assert b'Error' not in out,(line,out);return out
images=[];markers=[]
for n in range(3):
    path=smoke.WORK/str(n);_,image,_=make(path)
    marker=f'ONLY_DISK_{n}_TOPOLOGY_{CONTROLLER}'.encode();tmp=path/'marker.txt';tmp.write_bytes(marker)
    subprocess.run(['mcopy','-i',str(image)+'@@1048576',str(tmp),'::A'],check=True,capture_output=True)
    images.append(image);markers.append(marker)
def digest(p):return hashlib.sha256(p.read_bytes()).digest()
before=[digest(p) for p in images]
def drive(n):return f'if=none,id=flash{n},file={images[n]},format=raw'
def device(n):return f'usb-storage,id=stick{n},bus=usbtest.0,drive=flash{n},port='+('1.1','1.2.1','2')[n]
def tree():return ['-device',hub_device(CONTROLLER,0,'1'),'-device',hub_device(CONTROLLER,1,'1.2')]
def read(q,slot,n,ok=True):
    out=command(q,f'vcat /mnt/usb{slot}p1/A')
    assert (markers[n] in out)==ok,(slot,n,ok,out)
    if ok:assert all(m not in out for i,m in enumerate(markers) if i!=n),out
    return out
def stable(q,count=3):
    ds=disks(q);assert len(ds)==count
    nh=q.num("hub_count");raw=q.mem("hubs",C.sizeof(Hub)*nh) if nh else b""
    hs=[Hub.from_buffer_copy(raw,i*C.sizeof(Hub)).dev for i in range(nh)]
    assert len({(d.hc,d.address) for d in ds+hs})==count+len(hs)
    assert all(1<=d.address<=127 for d in ds+hs)
    assert all(d.generation and not d.write_allowed and not d.route_dead for d in ds)
    h=HC.from_buffer_copy(q.mem('hcs',C.sizeof(HC)));assert not h.dead
    return [d.generation for d in ds]
args=['-device',('usb-ehci' if CONTROLLER=='ehci' else 'pci-ohci' if CONTROLLER=='ohci' else 'piix3-usb-uhci')+',id=usbtest',*tree()]
for n in range(3):args+=['-drive',drive(n),'-device',device(n)]
print('Async TT TD-model topology (NOT wire proof):' if TT_LAB else 'HS-only LAB model topology (NO TT):' if CONTROLLER=='ehci' else 'FS hub topology:',CONTROLLER,'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
with smoke.Guest(disk=False,extra_args=args) as q:
    assert q.num('hc_count')==1 and q.num('hub_count')==2 and not q.num('hub_refused'), (q.num('hc_count'),q.num('hub_count'),q.num('hub_refused'),q.num('disk_count'),q.cmd('info usb'))
    assert q.num('ehci_count' if CONTROLLER=='ehci' else 'ohci_count' if CONTROLLER=='ohci' else 'uhci_count')==1
    info=q.cmd('info usb');assert info.count(b'Speed 480 Mb/s' if CONTROLLER=='ehci' else b'Speed 12 Mb/s')==(3 if TT_LAB else 5),info
    if TT_LAB:
        assert info.count(b'Speed 12 Mb/s')==2,info
        ds=disks(q);assert [d.speed for d in ds]==[0,0,2] and [d.in_mps for d in ds]==[64,64,512]
    previous=stable(q)
    for n in range(3):read(q,n,n)
    for attempt in range(12):
        command(q,'rescan');current=stable(q);assert current!=previous;previous=current
        if attempt in (0,5,11):
            for n in range(3):read(q,n,n)
    print('  nested + root disks, distinct data, 12 rescans/unique addresses/fresh RO PASS',flush=True)
    monitor(q,'device_del stick1');read(q,1,1,False);read(q,0,0);read(q,2,2)
    monitor(q,'drive_add 0 '+drive(1));monitor(q,'device_add '+device(1))
    read(q,1,1,False) # replacement does NOT revive a cached old mount
    command(q,'rescan');stable(q);read(q,1,1)
    print('  cached leaf unplug/reconnect: stale namespace refused until rescan, siblings live PASS',flush=True)
    # Remove the child first so QEMU can unregister hub ports. No OS I/O or
    # rescan in between: the old leaf's next access sees a missing ANCESTOR.
    monitor(q,'device_del stick1');monitor(q,'device_del hub1')
    read(q,1,1,False);read(q,0,0);read(q,2,2)
    assert not HC.from_buffer_copy(q.mem('hcs',C.sizeof(HC))).dead
    command(q,'rescan');stable(q,2);read(q,0,0);read(q,1,2)
    monitor(q,'device_add '+hub_device(CONTROLLER,1,'1.2'))
    monitor(q,'drive_add 0 '+drive(1));monitor(q,'device_add '+device(1))
    command(q,'rescan');stable(q);read(q,1,1)
    print('  intermediate hub unplug: ancestor-first refusal, native/root sibling unaffected PASS',flush=True)
    for n in (0,1):monitor(q,f'device_del stick{n}')
    for n in (1,0):monitor(q,f'device_del hub{n}')
    read(q,0,0,False);read(q,1,1,False);read(q,2,2)
    command(q,'rescan');stable(q,1);read(q,0,2)
    monitor(q,'device_add '+hub_device(CONTROLLER,0,'1'))
    monitor(q,'device_add '+hub_device(CONTROLLER,1,'1.2'))
    for n in (0,1):monitor(q,'drive_add 0 '+drive(n));monitor(q,'device_add '+device(n))
    command(q,'rescan');stable(q)
    for n in range(3):read(q,n,n)
    if CONTROLLER=='ehci':
        monitor(q,'device_add usb-mouse,usb_version=1,id=unsupported,bus=usbtest.0,port=1.3')
        command(q,'rescan');stable(q)
        info=q.cmd('info usb');assert b'Speed 12 Mb/s' in info or b'Speed 1.5 Mb/s' in info,info
        for n in range(3):read(q,n,n)
        monitor(q,'device_del unsupported');command(q,'rescan');stable(q)
        print('  unsupported USB1 HID stays unpublished/disabled, storage siblings keep working PASS',flush=True)
    (smoke.WORK/'topology-final.log').write_bytes(q.log.read_bytes())
    print('  root hub unplug/rebuild, address reuse, remapped disk indices/default RO PASS',flush=True)
assert [digest(p) for p in images]==before,'RO topology test changed a source image'
print('  all three WHOLE image SHA256 unchanged PASS',flush=True)
