#!/usr/bin/env python3
"""Measured-mouse RAM -> KvFS copy and same-path no-op, both native kernels.
Short-read/epoch/path/error injection is covered by the host backend model.
"""
from pathlib import Path
import os,sys,subprocess,ctypes as C,time,json,hashlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS
smoke.WORK=ROOT/'build/filemgr-copy'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'copy.img';KvFS(str(smoke.DISK),16)
class Entry(C.Structure):
    _fields_=[('name',C.c_char*128),('size',C.c_uint32),('flags',C.c_uint8*4),('year',C.c_uint16),('time',C.c_uint8*4)]
class Pane(C.Structure):
    _fields_=[('path',C.c_char*256),('items',Entry*512),('count',C.c_int),('sel',C.c_int),('scroll',C.c_int),('loaded',C.c_int),('total',C.c_uint32),('free',C.c_uint32)]
assert C.sizeof(Entry)==144 and C.sizeof(Pane)==74008

def panes(q):return (Pane*2).from_buffer_copy(q.mem('panes',C.sizeof(Pane)*2))
def status(q):return q.mem('fm_status',160).split(b'\0')[0]
def until(test,label):
    deadline=time.monotonic()+12
    while not test():
        assert time.monotonic()<deadline,label
        time.sleep(.025)
def persisted(name):
    e=next(e for e in KvFS(str(smoke.DISK)).listing() if e['name']==name)
    with smoke.DISK.open('rb') as f:f.seek(e['start']*512);return f.read(e['size'])
with smoke.Guest() as q:
    q.text('guimenu\n');until(lambda:q.num('running')==1,'GUI start');q.open_app(2)
    p=panes(q);assert bytes(p[0].path)==bytes(p[1].path)==b'/mnt/kv'
    q.widget(715,2);p=panes(q);assert bytes(p[0].path)==b'/mnt/ram' and p[0].count>0
    name=bytes(p[0].items[0].name).decode();expected=q.files()[name]
    q.widget(1000,2);q.widget(711,2)
    until(lambda:status(q).startswith(b'Copied:'),'native copy completion')
    assert persisted(name)==expected and q.files()[name]==expected
    # Rotate only active source pane to KvFS: both paths now match.
    q.widget(715,2);p=panes(q);assert bytes(p[0].path)==bytes(p[1].path)==b'/mnt/kv'
    assert p[0].sel==0 and q.num('fm_view_open')==0
    # Same row number on a changed listing is selection, never a double-click.
    q.widget(1000,2);assert q.num('fm_view_open')==0
    before=hashlib.sha256(smoke.DISK.read_bytes()).hexdigest();q.widget(711,2)
    until(lambda:status(q)==b'Source and target are the same','same-path refusal')
    assert hashlib.sha256(smoke.DISK.read_bytes()).hexdigest()==before
    # A real double-click on the unchanged listing must still open preview.
    q.widget(1000,2);assert q.num('fm_view_open')==0
    q.widget(1000,2)
    until(lambda:q.num('fm_view_open')==1,'unchanged-list double click')
    assert q.mem('fm_view_buf',min(len(expected),4095))==expected[:4095]
    q.widget(716,2);assert q.num('fm_view_open')==0
    q.close_app(2);q.exit_gui()
assert persisted(name)==expected
(smoke.WORK/'result.json').write_text(json.dumps({'arch64':smoke.IS64,'name':name,'bytes':len(expected),'sha256':hashlib.sha256(expected).hexdigest(),'checks':'mouse RAM -> KvFS copy persisted; source unchanged; same-path action writes no disk sectors; changed-list click selects only; real double-click previews; GUI/shell alive','negative_scope':'short-read/path/epoch/OOM/RO/partial-write faults host-only'},indent=2))
print('PASS file-manager copy','UEFI64' if smoke.IS64 else 'BIOS32',': RAM -> KvFS persists, source kept, same-path disk SHA unchanged, GUI exit',flush=True)
