#!/usr/bin/env python3
"""Measured mouse rename modal, production BIOS32/UEFI64; private images only."""
from pathlib import Path
import os,sys,subprocess,ctypes as C,time,json,hashlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS,DIR_SECTOR
SMALL=os.environ.get('FM_SMALL')=='1'
gui_command='vidmode 640 480\nguimenu\n' if SMALL else 'guimenu\n'
smoke.WORK=ROOT/('build/filemgr-rename-small' if SMALL else 'build/filemgr-rename')/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
class Entry(C.Structure):
    _fields_=[('name',C.c_char*128),('size',C.c_uint32),('flags',C.c_uint8*4),('year',C.c_uint16),('time',C.c_uint8*4)]
class Pane(C.Structure):
    _fields_=[('path',C.c_char*256),('items',Entry*512),('count',C.c_int),('sel',C.c_int),('scroll',C.c_int),('loaded',C.c_int),('total',C.c_uint32),('free',C.c_uint32)]
assert C.sizeof(Pane)==74008
image=smoke.WORK/'rename.img';smoke.DISK=image
payload=b'original\x00bytes\xff\n';k=KvFS(str(image),16);k.add('old.txt',payload,False);k.add('taken.txt',b'KEEP',False)
def panes(q):return (Pane*2).from_buffer_copy(q.mem('panes',C.sizeof(Pane)*2))
def text(q,name,n=128):return q.mem(name,n).split(b'\0')[0]
def status(q):return text(q,'fm_status',160)
def digest():return hashlib.sha256(image.read_bytes()).hexdigest()
def entries():return KvFS(str(image)).listing()
def data(name):
    e=next(e for e in entries() if e['name']==name)
    with image.open('rb') as f:f.seek(e['start']*512);return f.read(e['size'])
def until(test,label):
    deadline=time.monotonic()+10
    while not test():assert time.monotonic()<deadline,label;time.sleep(.025)
with smoke.Guest() as q:
    q.text(gui_command);until(lambda:q.num('running')==1,'GUI start');q.open_app(2)
    q.widget(1000,2);q.widget(719,2);assert q.num('fm_input_mode')==2 and text(q,'fm_input')==b'old.txt'
    before=digest();p=panes(q);q.widget(715,2);q.widget(1512,2)
    assert q.num('fm_active')==0 and bytes(panes(q)[0].path)==bytes(p[0].path)
    q.choose_name('cancelled.txt');q.widget(721,2);assert not q.num('fm_input_mode') and digest()==before
    q.widget(719,2);q.choose_name('taken.txt');q.widget(720,2)
    assert q.num('fm_input_mode')==2 and status(q)==b'Name exists; nothing replaced' and digest()==before
    q.key('ctrl-a');q.text('renamed.txt');q.widget(720,2);assert not q.num('fm_input_mode')
    assert data('renamed.txt')==payload and data('taken.txt')==b'KEEP' and 'old.txt' not in {e['name'] for e in entries()}
    p=panes(q);assert all(bytes(p[i].items[p[i].sel].name)==b'renamed.txt' for i in (0,1))
    q.widget(1000,2);assert not q.num('fm_view_open');q.widget(1000,2);assert q.num('fm_view_open')==1
    assert q.mem('fm_view_buf',len(payload))==payload;q.widget(716,2)
    before=digest();q.widget(719,2);q.widget(720,2);assert digest()==before and not q.num('fm_input_mode')
    q.widget(719,2);q.choose_name('x'*40);q.widget(720,2);assert q.num('fm_input_mode')==2 and digest()==before;q.widget(721,2)
    q.widget(719,2);q.choose_name('');q.key('alt-shift');q.text('afqk');assert text(q,'fm_input')=='файл'.encode()
    q.key('backspace');assert text(q,'fm_input')=='фай'.encode();q.text('k');q.key('alt-shift');q.text('.txt');q.widget(720,2)
    assert not q.num('fm_input_mode') and data('файл.txt')==payload
    before=digest();q.widget(719,2);q.close_app(2);assert not q.num('fm_input_mode') and digest()==before
    q.open_app(2);q.widget(715,2);p=panes(q);assert bytes(p[0].path)==b'/mnt/ram'
    old=bytes(p[0].items[0].name).decode();body=q.files()[old];q.widget(1000,2);q.widget(719,2);q.choose_name('ramnew.txt');q.widget(720,2)
    assert q.files()['ramnew.txt']==body and old not in q.files() and digest()==before
    q.widget(713,2);assert q.num('fm_input_mode')==1;q.choose_name('newdir');q.widget(720,2)
    assert q.num('fm_input_mode')==1 and digest()==before;q.widget(721,2)
    q.close_app(2);q.exit_gui();(smoke.WORK/'normal.serial.log').write_text(q.log.read_text(errors='replace'))
with smoke.Guest() as q:assert q.num('mounted')==1 and data('файл.txt')==payload and data('taken.txt')==b'KEEP'
image=smoke.WORK/'eio.img';k=KvFS(str(image),16);k.add('old.txt',payload,False)
config=smoke.WORK/'metadata.conf';config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\nimmediately = "on"\nsector = "'+str(DIR_SECTOR)+'"\n')
smoke.DISK=f'blkdebug:{config}:{image}'
with smoke.Guest() as q:
    q.text(gui_command);until(lambda:q.num('running')==1,'GUI EIO start');q.open_app(2)
    q.widget(719,2);q.choose_name('new.txt');q.widget(720,2)
    assert not q.num('fm_input_mode') and not q.num('mounted') and status(q).startswith(b'Outcome uncertain;')
    assert data('old.txt')==payload;q.close_app(2);q.exit_gui();(smoke.WORK/'eio.serial.log').write_text(q.log.read_text(errors='replace'))
smoke.DISK=image
with smoke.Guest() as q:assert q.num('mounted')==1 and data('old.txt')==payload
result={'arch64':smoke.IS64,'small_640x480':SMALL,'checks':'measured Rename/Apply/Cancel, modal pane+volume barrier, conflict no-replace/hash, same-name hash, overlong refusal, UTF8 RU keyboard/backspace, both pane refresh/reselect, post-rename double click, window-close cancel, mouse RAM rename, mkdir unsupported/cancel, real metadata EIO uncertainty/fence/cold remount','scope':'stale namespace/listing/path and after-publication fault injection host-only; no FAT/NTFS rename or crossvolume move; software only'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2))
print('PASS file-manager mouse rename','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
