#!/usr/bin/env python3
"""Real GUI KvFS unavailable/fenced != empty, explicit mount and format consent.
Only private regular images; no hardware qualification or secure-erase claim.
"""
from pathlib import Path
import os,sys,subprocess,time,json,hashlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS,KVFS_BASE,DIR_SECTOR,DATA_START
SMALL=os.environ.get('KVFS_SMALL')=='1'
smoke.WORK=ROOT/('build/kvfs-state-small' if SMALL else 'build/kvfs-state')/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
payload=b'preserve existing bytes\x00\xff';image=smoke.WORK/'foreign.img'
k=KvFS(str(image),16);k.add('old.txt',payload,False)
with image.open('r+b') as f:f.write(b'BOOT-REGION-MUST-STAY');f.seek(KVFS_BASE*512);f.write(b'OTHER')
smoke.DISK=image

def text(q,name,n=160):return q.mem(name,n).split(b'\0')[0]
def digest():return hashlib.sha256(image.read_bytes()).hexdigest()
def until(test,label):
    deadline=time.monotonic()+10
    while not test():assert time.monotonic()<deadline,label;time.sleep(.025)
def gui(q):
    q.text(('vidmode 640 480\n' if SMALL else '')+'guimenu\n');until(lambda:q.num('running')==1,'GUI start')
def data(name):
    e=next(e for e in KvFS(str(image)).listing() if e['name']==name)
    with image.open('rb') as f:f.seek(e['start']*512);return f.read(e['size'])
def capture(q,name):(smoke.WORK/name).write_text(q.log.read_text(errors='replace'))
with smoke.Guest(allow_unmounted=True) as q:
    assert not q.num('mounted');before=digest()
    assert 'no KvFS signature; existing data may be present' in q.log.read_text(errors='replace')
    offset=len(q.log.read_text(errors='replace'));q.text('df\ndls\ndcat old.txt\napps\ninstall readme.txt\nuninstall old.txt\n')
    output=q.log.read_text(errors='replace')[offset:]
    assert output.count('Formatting is not recovery;')==6 and 'not formatted' not in output and digest()==before
    gui(q);q.open_app(2);ram=q.files();q.widget(1000,2);q.widget(719,2);q.choose_name('snapshot.txt')
    q.open_app(8);generation=q.num('generation');q.widget(201,8)
    assert q.num('generation')!=generation and not q.num('mounted') and digest()==before
    assert text(q,'store_msg').startswith(b'No KvFS signature;')
    q.close_app(8);q.open_app(2);q.widget(720,2)
    assert not q.num('fm_input_mode') and text(q,'fm_status')==b'Listing changed; reopen the action' and q.files()==ram
    q.close_app(2);q.open_app(8)
    for action in ('cancel','escape','close'):
        q.widget(200,8);assert q.num('store_format_confirm')==1 and digest()==before
        until(lambda:any(h[4]==202 for h in q.hits(8)),'confirmation widgets')
        assert not any(h[4] in (200,201,500,501) for h in q.hits(8))
        q.key('ret');assert q.num('store_format_confirm')==1 and digest()==before
        if action=='cancel':q.widget(203,8)
        elif action=='escape':q.key('esc')
        else:q.close_app(8)
        assert not q.num('store_format_confirm') and digest()==before
        if action=='close':q.open_app(8)
    raw=image.read_bytes();q.widget(200,8);q.widget(202,8)
    assert not q.num('store_format_confirm') and q.num('mounted')==1 and text(q,'store_msg')==b'KvFS formatted'
    after=image.read_bytes();assert raw[:KVFS_BASE*512]==after[:KVFS_BASE*512] and raw[DATA_START*512:]==after[DATA_START*512:]
    assert KvFS(str(image)).listing()==[]
    q.close_app(8);q.open_app(2);q.widget(715,2) # newly registered KvFS is reachable
    assert text(q,'panes',256)==b'/mnt/kv';q.close_app(2);q.exit_gui();capture(q,'format.serial.log')
with smoke.Guest() as q:assert q.num('mounted')==1 and KvFS(str(image)).listing()==[]
# A real pre-publication metadata EIO. Recovery is an explicit read-only check,
# followed by inspection and a DIFFERENT fresh user action, never an auto retry.
image=smoke.WORK/'fenced.img';k=KvFS(str(image),16);k.add('old.txt',payload,False)
config=smoke.WORK/'metadata.conf';config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\nimmediately = "on"\nsector = "'+str(DIR_SECTOR)+'"\n')
smoke.DISK=f'blkdebug:{config}:{image}'
with smoke.Guest() as q:
    gui(q);q.open_app(2);q.widget(719,2);q.choose_name('new.txt');generation=q.num('generation');q.widget(720,2)
    assert not q.num('mounted') and not q.num('fm_input_mode') and q.num('generation')!=generation
    assert text(q,'fm_status').startswith(b'Outcome uncertain;') and data('old.txt')==payload
    q.open_app(8);before=digest();q.widget(201,8)
    assert q.num('mounted')==1 and text(q,'store_msg')==b'KvFS checked and mounted' and digest()==before
    assert {e['name'] for e in KvFS(str(image)).listing()}=={'old.txt'}
    q.close_app(8);q.open_app(2);q.widget(714,2);q.widget(719,2)
    assert text(q,'fm_input')==b'old.txt';q.choose_name('checked.txt');q.widget(720,2)
    assert text(q,'fm_status')==b'Renamed' and data('checked.txt')==payload
    q.close_app(2);q.exit_gui();capture(q,'remount.serial.log')
smoke.DISK=image
with smoke.Guest() as q:assert q.num('mounted')==1 and data('checked.txt')==payload
with smoke.Guest(disk=False) as q:
    gui(q);q.open_app(8);assert not any(h[4] in (200,201,202,203) for h in q.hits(8))
    q.close_app(8);q.exit_gui();capture(q,'diskless.serial.log')
result={'arch64':smoke.IS64,'small_640x480':SMALL,'checks':'foreign signature retains all bytes; six truthful shell refusals; failed read-only mount invalidates FM snapshot; format first click/Enter/cancel/Esc/close write nothing; explicit erase affects only KvFS metadata, cold mount and VFS registration; real metadata EIO namespace fence, explicit read-only validated remount, inspect then fresh different action, cold persistence; diskless no format/mount controls','scope':'software drive/capacity/namespace guard only; physical hotplug and after-publication EIO host-only; format is not secure erase; no hardware/TT recovery claim'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2))
print('PASS KvFS truthful state','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
