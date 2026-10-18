#!/usr/bin/env python3
"""Production-kernel KvFS COW failures: backend flush EIO and metadata EIO.
Private file images only. Checks GUI dirty/history and cold remount contents.
"""
from pathlib import Path
import os,sys,subprocess,ctypes as C,time
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
from text_model import editor_type
Editor=editor_type(smoke.IS64)
assert 'usb_test_authorize' not in smoke.symbols,'must test the normal production configuration'
sys.path.insert(0,str(ROOT/'sdk'))
from mkdisk import KvFS
smoke.WORK=ROOT/'build/kvfs-write'/('uefi64' if smoke.IS64 else 'bios32');smoke.WORK.mkdir(parents=True,exist_ok=True)
def state(q):return Editor.from_buffer_copy(q.mem('note_editor',C.sizeof(Editor)))
def read_file(path):
    k=KvFS(str(path));e=next(e for e in k.listing() if e['name']=='old.txt')
    with path.open('rb') as f:f.seek(e['start']*512);return f.read(e['size'])
print('KvFS write faults:','UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
for case,event,sector,ready in [('body-flush','flush_to_disk',None,1),('directory','write_aio',2049,0)]:
    image=smoke.WORK/(case+'.img');k=KvFS(str(image),16);k.add('old.txt',b'old',False)
    config=smoke.WORK/(case+'.conf')
    config.write_text('[inject-error]\nevent = "'+event+'"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n'+(f'sector = "{sector}"\n' if sector is not None else ''))
    smoke.DISK=f'blkdebug:{config}:{image}'
    with smoke.Guest() as q:
        q.text('guimenu\n');time.sleep(1);q.open_app(7)
        q.widget(41,7);q.choose_name('old.txt');q.widget(82,7);assert q.document()['text']==b'old'
        q.key('ctrl-a');q.text('new');before=bytes(state(q).undo);q.widget(42,7)
        s=state(q);assert q.document()['text']==b'new' and q.document()['dirty']
        assert s.disk_failed and not s.disk_valid and bytes(s.undo)==before
        assert q.num('mounted')==ready
        assert q.files()['old.txt']==b'new'
        q.screenshot(smoke.WORK/(case+'.png'))
        (smoke.WORK/(case+'.log')).write_text(q.log.read_text(errors='replace'))
    assert read_file(image)==b'old','old live extent/metadata was lost on failed save'
    smoke.DISK=image
    with smoke.Guest() as q:
        q.text('guimenu\n');time.sleep(1);q.open_app(7)
        q.widget(41,7);q.choose_name('old.txt');q.widget(82,7)
        assert q.document()['text']==b'old' and not q.document()['dirty']
    print('  '+case+': error propagated, RAM/current document/history kept, fence='+str(not ready)+', cold old file survives PASS',flush=True)
