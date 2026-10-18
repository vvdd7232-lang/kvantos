#!/usr/bin/env python3
"""Production no-replace rename: RAM full table, KvFS metadata + cold mount/EIO.
Only private file images; no cross-volume fallback. Mouse dialog is not tested here.
"""
from pathlib import Path
import os,sys,subprocess,time,json,hashlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS,DIR_SECTOR
smoke.WORK=ROOT/'build/native-rename'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
image=smoke.WORK/'rename.img';smoke.DISK=image
payload=b'native\x00rename\xff\n';k=KvFS(str(image),16);k.add('old.kapp',payload,True);k.add('keep.txt',b'KEEP',False)

def command(q,text,message):
    offset=len(q.log.read_text(errors='replace'));q.text(text+'\n');deadline=time.monotonic()+8
    while message not in q.log.read_text(errors='replace')[offset:]:
        assert time.monotonic()<deadline,(text,message,q.log.read_text(errors='replace')[-600:]);time.sleep(.025)
def entries():return KvFS(str(image)).listing()
def data(name):
    e=next(e for e in entries() if e['name']==name)
    with image.open('rb') as f:f.seek(e['start']*512);return f.read(e['size'])
def digest():return hashlib.sha256(image.read_bytes()).hexdigest()
with smoke.Guest() as q:
    original=q.files();source='readme.txt';assert source in original
    for i in range(32-len(original)):q.text(f'touch f{i}\n')
    assert len(q.files())==32
    command(q,'mv readme.txt renamed.txt',"'readme.txt' renamed to 'renamed.txt'")
    assert len(q.files())==32 and 'readme.txt' not in q.files() and q.files()['renamed.txt']==original[source]
    before=q.files();command(q,'mv renamed.txt license.txt','target exists; neither file replaced');assert q.files()==before
    ram_snapshot=q.files()
    old=next(e for e in entries() if e['name']=='old.kapp');before=image.read_bytes()
    command(q,'mv /mnt/kv/old.kapp /mnt/kv/new.kapp',"'/mnt/kv/old.kapp' renamed to '/mnt/kv/new.kapp'")
    new=next(e for e in entries() if e['name']=='new.kapp');assert {**old,'name':'new.kapp'}==new and data('new.kapp')==payload
    after=image.read_bytes();a=DIR_SECTOR*512;b=a+512;assert before[:a]==after[:a] and before[b:]==after[b:]
    for text,message in [('mv /mnt/kv/new.kapp /mnt/kv/keep.txt','target exists; neither file replaced'),('mv /mnt/kv/new.kapp /mnt/ram/unused.kapp','cross-volume move unsupported'),('mv /mnt/kv/new.kapp /mnt/kv/new.kapp',"'/mnt/kv/new.kapp' renamed to '/mnt/kv/new.kapp'")]:
        before=digest();command(q,text,message);assert digest()==before
    assert q.files()==ram_snapshot
    (smoke.WORK/'normal.serial.log').write_text(q.log.read_text(errors='replace'))
with smoke.Guest() as q:
    assert q.num('mounted')==1 and data('new.kapp')==payload and data('keep.txt')==b'KEEP'
    assert {e['name'] for e in entries()}=={'new.kapp','keep.txt'}
    (smoke.WORK/'cold.serial.log').write_text(q.log.read_text(errors='replace'))
# Real blkdebug pre-publication metadata EIO; post-publication failure is host-only.
image=smoke.WORK/'eio.img';k=KvFS(str(image),16);k.add('old.kapp',payload,True)
config=smoke.WORK/'metadata.conf';config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\nimmediately = "on"\nsector = "'+str(DIR_SECTOR)+'"\n')
smoke.DISK=f'blkdebug:{config}:{image}'
with smoke.Guest() as q:
    command(q,'mv /mnt/kv/old.kapp /mnt/kv/new.kapp','metadata outcome uncertain; remount and inspect, do not retry')
    assert q.num('mounted')==0 and data('old.kapp')==payload
    (smoke.WORK/'eio.serial.log').write_text(q.log.read_text(errors='replace'))
smoke.DISK=image
with smoke.Guest() as q:assert q.num('mounted')==1 and data('old.kapp')==payload
result={'arch64':smoke.IS64,'checks':'RAM full-table in-place/no-replace; KvFS single metadata sector/data+extent+flags preserved; conflicts/crossvolume/samepath disk hash unchanged; cold remount; real blkdebug metadata EIO unmount + validated cold mount','scope':'software only; after-publication ambiguity/invalid names/OOM/RO unsupported host-only; mouse rename UI not yet implemented'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2))
print('PASS native rename','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
