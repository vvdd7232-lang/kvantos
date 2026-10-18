#!/usr/bin/env python3
"""Two actual QEMU HS root HCs; debugger-timed detach between link check and
schedule-off. No product hooks, RAM edits, HS hub/TT or physical claims.
Private regular-file fixtures only. Requires a fresh NORMAL release build.
"""
from pathlib import Path
import hashlib, os, shutil, subprocess, sys, time
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
from mkusb import make
from qemu_break import Breakpoint
smoke.WORK=ROOT/'build/ehci-isolation'/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
raw,first,proof=make(smoke.WORK);second=smoke.WORK/'second.img';shutil.copyfile(first,second)
source=smoke.WORK/'second.c';source.write_bytes(proof.replace(b'42',b'77'))
subprocess.run(['mcopy','-o','-i',str(second)+'@@1048576',str(source),'::HELLO.C'],check=True)
def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
before={p:digest(p) for p in (raw,first,second)}
args=['-gdb','unix:'+str(smoke.WORK/'debug.sock')+',server=on,wait=off']
for i,image in enumerate((first,second)):
    args+=['-device',f'usb-ehci,id=hc{i}','-drive',f'file={image},format=raw,if=none,id=flash{i}',
           '-device',f'usb-storage,id=stick{i},bus=hc{i}.0,drive=flash{i},port=1']
def output(q,start,timeout=10):
    end=time.monotonic()+timeout
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
    return [(data[i*stride+ptr+4],data[i*stride+ptr+8]) for i in range(2)] # dead,pool
print('EHCI isolation:', 'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
(smoke.WORK/'debug.sock').unlink(missing_ok=True)
with smoke.Guest(disk=False,extra_args=args) as q:
    assert q.num('hc_count')==q.num('ehci_count')==q.num('disk_count')==2
    assert q.num('ehci_used')==2
    hs=controllers(q);assert hs[0][0]==hs[1][0]==0 and hs[0][1]!=hs[1][1],hs
    assert q.cmd('info usb').count(b'Speed 480 Mb/s')==2
    assert b'Copied ' in command(q,'vcp /mnt/usb1p1/HELLO.C /mnt/ram/keep.c')
    expected=q.files()['keep.c'];assert expected==source.read_bytes()
    # Source 0 is not the previous source: force real BOT, not a reused cache hit.
    bp=Breakpoint(q,smoke.WORK/'debug.sock','ehci_async_off')
    start=len(q.log.read_bytes());q.text('vcp /mnt/usb0p1/HUGE.C /mnt/ram/keep.c\n');bp.wait()
    assert b'Error' not in q.cmd('device_del stick0')
    bp.resume();reply=output(q,start)
    assert b'Copied ' not in reply,reply
    assert q.files()['keep.c']==expected,'failed source replaced the RAM destination'
    hs=controllers(q);assert hs[0][0]==1 and hs[1][0]==0,hs
    assert b'Copied ' in command(q,'vcp /mnt/usb1p1/HUGE.C /mnt/ram/live.c')
    assert q.files()['live.c'].endswith(proof.replace(b'\n',b'\r\n'))
    assert b'42' in q.files()['live.c'] and len(q.files()['live.c'])>16000
    # Removing the failed HC's device does not revoke another HC's root port.
    assert controllers(q)[1][0]==0
    (smoke.WORK/'isolation.serial.log').write_bytes(q.log.read_bytes())
assert all(digest(p)==h for p,h in before.items()),'unexpected fixture write'
print('PASS two HS HCs/private leases; real detach in link-check race -> HC0 fenced, RAM destination intact; HC1 still reads >16KiB; all source SHA unchanged',flush=True)
