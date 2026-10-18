#!/usr/bin/env python3
"""Actual production shell VFS copy and real ATA/metadata EIO, files only."""
from pathlib import Path
import os,sys,subprocess,time,json,hashlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS,DATA_START,DIR_SECTOR
smoke.WORK=ROOT/'build/shell-vcp'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
image=smoke.WORK/'normal.img';smoke.DISK=image
payload=b'shell copy\x00payload\xff';k=KvFS(str(image),16);k.add('source.bin',payload,False);k.add('keep.bin',b'KEEP',False);k.add('empty.bin',b'',False)
def digest():return hashlib.sha256(image.read_bytes()).hexdigest()
def data(name):
    e=next(e for e in KvFS(str(image)).listing() if e['name']==name)
    with image.open('rb') as f:f.seek(e['start']*512);return f.read(e['size'])
def command(q,cmd,message):
    offset=len(q.log.read_text(errors='replace'));q.text(cmd+'\n');deadline=time.monotonic()+10
    while message not in q.log.read_text(errors='replace')[offset:]:
        assert time.monotonic()<deadline,(cmd,message,q.log.read_text(errors='replace')[-600:]);time.sleep(.025)
    return q.log.read_text(errors='replace')[offset:]
def capture(q,name):(smoke.WORK/name).write_text(q.log.read_text(errors='replace'))
with smoke.Guest() as q:
    before=digest();source=q.files()['readme.txt']
    command(q,'vcp /mnt/kv/source.bin /mnt/ram/license.txt',f'Copied {len(payload)} bytes');assert q.files()['license.txt']==payload and digest()==before
    for to in ('/mnt/kv/source.bin','/mnt/kv'):
        command(q,'vcp /mnt/kv/source.bin '+to,'Source and target are the same; unchanged');assert digest()==before
    command(q,'vcp /mnt/ram/license.txt /mnt/kv/keep.bin',f'Copied {len(payload)} bytes');assert data('keep.bin')==payload
    command(q,'vcd /mnt/ram','  /mnt/ram')
    command(q,'vcp license.txt /mnt/kv/relative.bin',f'Copied {len(payload)} bytes');assert data('relative.bin')==payload
    before=digest();ram=q.files();command(q,'vcp license.txt /mnt/ram','Source and target are the same; unchanged');assert digest()==before and q.files()==ram
    command(q,'vcp missing /mnt/kv/keep.bin','Source not found:');assert digest()==before
    command(q,'vcp /mnt/kv/empty.bin license.txt','Copied 0 bytes');assert q.files()['license.txt']==b'' and digest()==before
    command(q,'vcp license.txt /mnt/kv/keep.bin','Copied 0 bytes');assert data('keep.bin')==b''
    before=digest();command(q,'vcp readme.txt /mnt/kv/'+'x'*40,'Write error; source kept, inspect target');assert digest()==before and q.files()['readme.txt']==source
    capture(q,'normal.serial.log')
with smoke.Guest() as q:assert data('source.bin')==data('relative.bin')==payload and data('keep.bin')==b''
for case,event,sector,ready in [('read','read_aio',DATA_START,1),('body-flush','flush_to_disk',None,1),('metadata','write_aio',DIR_SECTOR,0)]:
    image=smoke.WORK/(case+'.img');k=KvFS(str(image),16);k.add('keep.bin',b'KEEP',False)
    config=smoke.WORK/(case+'.conf');config.write_text('[inject-error]\nevent = "'+event+'"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n'+(f'sector = "{sector}"\n' if sector is not None else ''))
    smoke.DISK=f'blkdebug:{config}:{image}'
    with smoke.Guest() as q:
        ram=q.files();before=digest()
        cmd='vcp /mnt/kv/keep.bin /mnt/ram/license.txt' if case=='read' else 'vcp /mnt/ram/readme.txt /mnt/kv/keep.bin'
        message='Incomplete read or storage changed; target not written' if case=='read' else 'Storage changed during write; source kept, inspect target' if case=='metadata' else 'Write error; source kept, inspect target'
        output=command(q,cmd,message);assert 'Copied ' not in output and q.files()==ram and q.num('mounted')==ready and data('keep.bin')==b'KEEP'
        if case=='read':assert digest()==before
        capture(q,case+'.serial.log')
    smoke.DISK=image
    with smoke.Guest() as q:assert q.num('mounted')==1 and data('keep.bin')==b'KEEP'
result={'arch64':smoke.IS64,'checks':'actual shell binary KvFS->RAM->KvFS and relative/directory target, source retention, samepath whole-image hash unchanged, empty copy and invalid KvFS name refusal, cold persistence; real read EIO does not write target, body flush EIO retains published file, metadata EIO fences and is not success, all cold recover','scope':'short/overcount, namespace injection, OOM/RO/4MiB/long-CWD/path-join boundary tests host-only; no aliases canonicalization/atomic multiwriter/physical storage qualification'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2));print('PASS shell vcp','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
