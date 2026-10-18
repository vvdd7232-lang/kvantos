#!/usr/bin/env python3
"""Replay archived private fixture from the observed x64 TT Save As refusal.
Argument: proof directory containing source.img.gz/observation.json/expected-namespace.json.
Checks cold recovery via NORMAL kernel before namespace, RO restored, fsck,
old-or-complete-new namespace and unchanged bytes outside the FAT partition.
Not a reproduction of the original transient timing failure or hardware proof.
"""
from pathlib import Path
import gzip,hashlib,json,os,shutil,stat,struct,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
assert len(sys.argv)==2,'usage: ehci-tt-recovery-smoke.py PROOF_DIRECTORY'
proof=Path(sys.argv[1]).resolve()
CONTROLLER=os.environ.get('USB_RECOVERY_CONTROLLER','ehci')
assert CONTROLLER in ('ehci','ohci')
if CONTROLLER=='ehci':assert os.environ.get('USB_TT_LAB')=='1' and os.environ.get('USB_HS_LAB')=='1'
else:assert os.environ.get('USB_TT_LAB')!='1','stock recovery is NOT TT qualification'
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__,str(proof)],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
from usb_topology import hub_device
smoke.WORK=ROOT/'build/recovery-fixture'/CONTROLLER/proof.name/('uefi64' if smoke.IS64 else 'bios32');smoke.WORK.mkdir(parents=True,exist_ok=True)
obs=json.loads((proof/'observation.json').read_text());expected={k:None if v is None else bytes.fromhex(v) for k,v in json.loads((proof/'expected-namespace.json').read_text()).items()}
archive=proof/'source.img.gz';assert stat.S_ISREG(archive.stat().st_mode)
source=smoke.WORK/'recovery.img'
with gzip.open(archive,'rb') as src,source.open('wb') as dst:shutil.copyfileobj(src,dst)
def digest(data):return hashlib.sha256(data).hexdigest()
before=source.read_bytes();assert digest(before)==obs['source_sha256'] and len(before)==obs['bytes']
first=obs['first'];total=obs['total'];assert 0<first<first+total<=len(before)//512
outside=(digest(before[:first*512]),digest(before[(first+total)*512:]))
if CONTROLLER=='ehci':
    args=['-device','ich9-usb-ehci1,id=usbtest',
          '-device','pci-ohci,id=comp0,masterbus=usbtest.0,firstport=0',
          '-device','pci-ohci,id=comp1,masterbus=usbtest.0,firstport=3']
    root='3'
else:
    args=['-device','pci-ohci,id=usbtest'];root='1'
args+=['-device',hub_device(CONTROLLER,0,root,False),'-device',hub_device(CONTROLLER,1,root+'.1'),
       '-drive',f'file={source},format=raw,if=none,id=flash',
       '-device','usb-storage,id=stick,bus=usbtest.0,drive=flash,port='+root+'.1.1']
with smoke.Guest(disk=False,extra_args=args) as q:
    assert q.num('disk_count')==1 and q.num('hub_count')==2
    at=len(q.log.read_bytes());q.text('vls /mnt/usb0p2\n');end=time.monotonic()+30
    while b'kvant:/$' not in q.log.read_bytes()[at:]:
        assert time.monotonic()<end,'cold recovery/list timeout'
        time.sleep(.03)
    b=q.mem('disks',48 if smoke.IS64 else 40)
    assert not b[24 if smoke.IS64 else 20],'recovery leaked a user write grant'
    assert not b[43 if smoke.IS64 else 39],'fixture must actually be FS'
    (smoke.WORK/'recovery.serial.log').write_bytes(q.log.read_bytes())
after=source.read_bytes();assert (digest(after[:first*512]),digest(after[(first+total)*512:]))==outside
part=smoke.WORK/'part.img';part.write_bytes(after[first*512:(first+total)*512])
fsck=subprocess.run(['/sbin/fsck.fat','-n',str(part)],capture_output=True,text=True);assert not fsck.returncode,(fsck.stdout,fsck.stderr)
out=smoke.WORK/'export'
if out.exists():shutil.rmtree(out)
out.mkdir();subprocess.run(['mcopy','-s','-i',str(part),'::*',str(out)],capture_output=True,check=True)
actual={str(p.relative_to(out)):None if p.is_dir() else p.read_bytes() for p in out.rglob('*') if p.name.upper()!='KVANTWAL.SYS'}
complete=dict(expected);complete[obs.get('complete_file','Empty long directory/Large.txt')]=(b'a'*63+b'\n')*1023+b'z'*63
assert actual in (expected,complete),[(k,None if v is None else len(v)) for k,v in actual.items()]
print('PASS archived-state cold recovery',CONTROLLER,'UEFI64' if smoke.IS64 else 'BIOS32',':', 'old' if actual==expected else 'complete-new','namespace, default RO, fsck, outside-partition SHA unchanged; not timing-failure reproduction',flush=True)
