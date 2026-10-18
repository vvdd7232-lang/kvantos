#!/usr/bin/env python3
"""Production measured GUI: stale listings before actions and delete snapshots.
VFS changes come from explicit read-only Programs remount, not guest-memory writes.
"""
from pathlib import Path
import os,sys,subprocess,time,json,hashlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS,DIR_SECTOR
SMALL=os.environ.get('FM_SMALL')=='1'
smoke.WORK=ROOT/('build/filemgr-namespace-small' if SMALL else 'build/filemgr-namespace')/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
image=smoke.WORK/'namespace.img';smoke.DISK=image;payload=b'namespace source\x00\xff'
k=KvFS(str(image),16);k.add('old.txt',payload,False);k.add('victim.txt',b'VICTIM',False)
def digest():return hashlib.sha256(image.read_bytes()).hexdigest()
def text(q,n,size=160):return q.mem(n,size).split(b'\0')[0]
def entries():return {e['name'] for e in KvFS(str(image)).listing()}
def data(name):
    e=next(e for e in KvFS(str(image)).listing() if e['name']==name)
    with image.open('rb') as f:f.seek(e['start']*512);return f.read(e['size'])
def until(test,label):
    deadline=time.monotonic()+10
    while not test():assert time.monotonic()<deadline,label;time.sleep(.025)
def gui(q):
    q.text(('vidmode 640 480\n' if SMALL else '')+'guimenu\n');until(lambda:q.num('running')==1,'GUI start')
def remount(q):
    generation=q.num('generation');q.open_app(8);q.widget(201,8)
    assert q.num('mounted')==1 and q.num('generation')!=generation
    q.close_app(8);q.open_app(2)
with smoke.Guest() as q:
    gui(q);q.open_app(2);before=digest();ram=q.files();remount(q)
    until(lambda:not any(h[4]>=1000 for h in q.hits(2)),'stale rows removed')
    for widget in (719,711,712,713):
        q.widget(widget,2);assert not q.num('fm_input_mode') and not q.num('fm_confirm') and digest()==before
        assert text(q,'fm_status')==b'Listing changed or unavailable; refresh'
    q.key('ret');assert not q.num('fm_view_open') and digest()==before and q.files()==ram
    q.widget(714,2);q.widget(712,2);assert q.num('fm_confirm')==1
    remount(q);q.widget(717,2)
    assert not q.num('fm_confirm') and text(q,'fm_status')==b'Listing changed; reopen the action' and digest()==before
    q.widget(714,2);q.widget(719,2);q.choose_name('renamed.txt');q.widget(720,2)
    assert data('renamed.txt')==payload and entries()=={'renamed.txt','victim.txt'}
    q.widget(1001,2);before=digest();q.widget(712,2);q.widget(718,2);assert digest()==before
    q.widget(712,2);q.widget(717,2);assert entries()=={'renamed.txt'} and data('renamed.txt')==payload
    q.close_app(2);q.exit_gui();(smoke.WORK/'normal.serial.log').write_text(q.log.read_text(errors='replace'))
with smoke.Guest() as q:assert q.num('mounted')==1 and entries()=={'renamed.txt'} and data('renamed.txt')==payload
image=smoke.WORK/'delete-eio.img';k=KvFS(str(image),16);k.add('old.txt',payload,False)
config=smoke.WORK/'metadata.conf';config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\nimmediately = "on"\nsector = "'+str(DIR_SECTOR)+'"\n')
smoke.DISK=f'blkdebug:{config}:{image}'
with smoke.Guest() as q:
    gui(q);q.open_app(2);q.widget(712,2);q.widget(717,2)
    assert not q.num('mounted') and not q.num('fm_confirm') and text(q,'fm_status').startswith(b'Delete outcome uncertain;')
    assert data('old.txt')==payload;q.close_app(2);q.exit_gui();(smoke.WORK/'delete-eio.serial.log').write_text(q.log.read_text(errors='replace'))
smoke.DISK=image
with smoke.Guest() as q:assert q.num('mounted')==1 and data('old.txt')==payload
result={'arch64':smoke.IS64,'small_640x480':SMALL,'checks':'explicit read-only remount invalidates old rendered listings; stale preview/copy/rename/delete/mkdir refuse, no rows or writes; fresh Refresh restores actions; old delete confirmation rejected after remount; fresh rename/delete and cancelled delete preserve other data, cold persistence; real delete metadata EIO reports uncertainty/fence/cold mount','scope':'generation during readdir/space, malformed entries, preview overcount and after-publication delete fault host-only; software namespace, not physical hotplug qualification'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2))
print('PASS listing namespace','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
