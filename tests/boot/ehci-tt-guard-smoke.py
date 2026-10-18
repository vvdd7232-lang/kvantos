#!/usr/bin/env python3
"""Explicit TD-level TT LAB, NORMAL kernel, two HC. Private images only.
Wrong-route case deliberately edits one guest QH via debugger (not hardware).
Disconnect case observes live SplitXState/active qTD, then removes FS device.
This is not TT wire/timing/physical qualification.
"""
from pathlib import Path
import hashlib,os,shutil,struct,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
assert os.environ.get('USB_TT_LAB')=='1' and os.environ.get('USB_HS_LAB')=='1'
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
from mkusb import make
from qemu_break import Breakpoint
from usb_topology import hub_device
smoke.WORK=ROOT/'build/ehci-tt-guard'/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
raw,first,proof=make(smoke.WORK);second=smoke.WORK/'second.img';shutil.copyfile(first,second)
source=smoke.WORK/'second.c';source.write_bytes(proof.replace(b'42',b'77'))
subprocess.run(['mcopy','-o','-i',str(second)+'@@1048576',str(source),'::HELLO.C'],check=True)
def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
before={p:digest(p) for p in (raw,first,second)}
def output(q,start):
    end=time.monotonic()+15
    while time.monotonic()<end:
        data=q.log.read_bytes()[start:]
        if b'kvant:/$' in data:return data
        time.sleep(.03)
    raise AssertionError(('shell completion timeout',q.log.read_bytes()[-2000:]))
def command(q,text):
    start=len(q.log.read_bytes());q.text(text+'\n');return output(q,start)
def controllers(q):
    ptr=8 if smoke.IS64 else 4;stride=24 if smoke.IS64 else 20
    data=q.mem('hcs',2*stride)
    return [(data[i*stride+ptr+4],data[i*stride+ptr+8]) for i in range(2)]
def advance(bp):
    assert bp.request('s').startswith((b'T05',b'S05'))
    bp.s.sendall(b'$c#63');assert bp.s.recv(1)==b'+'
args=['-gdb','unix:'+str(smoke.WORK/'debug.sock')+',server=on,wait=off',
      '-device','usb-ehci,id=usbtest','-device',hub_device('ehci',0,'1',False),
      '-device',hub_device('ehci',1,'1.1'),
      '-drive',f'file={first},format=raw,if=none,id=flash0',
      '-device','usb-storage,id=stick0,bus=usbtest.0,drive=flash0,port=1.1.1',
      '-device','usb-ehci,id=hc1','-drive',f'file={second},format=raw,if=none,id=flash1',
      '-device','usb-storage,id=stick1,bus=hc1.0,drive=flash1,port=1']
print('TT guard LAB:', 'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
for case in ('bad-route','inflight-disconnect'):
    (smoke.WORK/'debug.sock').unlink(missing_ok=True)
    with smoke.Guest(disk=False,extra_args=args) as q:
        assert q.num('hc_count')==q.num('ehci_count')==q.num('disk_count')==2
        assert q.num('hub_count')==2 and q.num('ehci_used')==2
        assert controllers(q)[0][0]==controllers(q)[1][0]==0
        info=q.cmd('info usb');assert info.count(b'Speed 12 Mb/s')==1,info
        assert b'Copied ' in command(q,'vcp /mnt/usb1p1/HELLO.C /mnt/ram/keep.c')
        expected=q.files()['keep.c'];assert expected==source.read_bytes()
        bp=Breakpoint(q,smoke.WORK/'debug.sock','ehci_link')
        start=len(q.log.read_bytes());q.text('vcp /mnt/usb0p1/HUGE.C /mnt/ram/keep.c\n')
        qh=q.symbols['ehci_pool']+4096;td=qh+96
        deadline=time.monotonic()+40;traps=0
        while True:
            bp.wait();traps+=1
            fields=struct.unpack('<4I',q.mem(qh,16));token=struct.unpack('<I',q.mem(td+8,4))[0]
            overlay=struct.unpack('<I',q.mem(qh+24,4))[0]
            fs=((fields[1]>>12)&3)==0 and (fields[1]>>8)&15
            if fs and token&0x80 and (not overlay if case=='bad-route' else overlay&2):break
            assert time.monotonic()<deadline,(case,traps,fields,token,overlay)
            advance(bp)
        if case=='bad-route':
            bad=struct.pack('<I',fields[2]^(1<<16)).hex()
            assert bp.request(f'M{qh+8:x},4:{bad}')==b'OK'
        else:
            assert b'Error' not in q.cmd('device_del stick0')
        bp.resume();reply=output(q,start)
        assert b'Copied ' not in reply,reply
        assert q.files()['keep.c']==expected,'failed TT source replaced RAM destination'
        hs=controllers(q);assert hs[0][0]==1 and hs[1][0]==0,hs
        assert b'Copied ' in command(q,'vcp /mnt/usb1p1/HUGE.C /mnt/ram/live.c')
        assert len(q.files()['live.c'])>16000 and b'42' in q.files()['live.c']
        assert controllers(q)[1][0]==0
        (smoke.WORK/(case+'.serial.log')).write_bytes(q.log.read_bytes())
        trace=max(smoke.WORK.glob('tt-model-*.trace'),key=lambda p:p.stat().st_mtime_ns).read_text()
        if case=='bad-route':assert 'BAD route/QH' in trace,trace[-1000:]
        else:assert 'CS NYET' in trace,trace[-1000:]
        print(' ',case,': live QH observed; HC0 fenced, caller/RAM kept, HC1 reads >16KiB, traps',traps,'PASS',flush=True)
assert all(digest(p)==h for p,h in before.items()),'unexpected fixture write'
print('PASS TT LAB native fault guards; all source whole-image SHA unchanged (NOT hardware proof)',flush=True)
