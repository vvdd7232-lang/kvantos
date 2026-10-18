#!/usr/bin/env python3
"""Empty != unavailable in real GUI and vls, private ATA fixture and EIO."""
from pathlib import Path
import os,sys,subprocess,time,json,hashlib,ctypes as C,struct
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS,DIR_SECTOR
SMALL=os.environ.get('LIST_SMALL')=='1'
smoke.WORK=ROOT/('build/listing-status-small' if SMALL else 'build/listing-status')/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
image=smoke.WORK/'empty.img';smoke.DISK=image;KvFS(str(image),16)
class Entry(C.Structure):_fields_=[('name',C.c_char*128),('size',C.c_uint32),('flags',C.c_uint8*4),('year',C.c_uint16),('time',C.c_uint8*4)]
class Pane(C.Structure):_fields_=[('path',C.c_char*256),('items',Entry*512),('count',C.c_int),('sel',C.c_int),('scroll',C.c_int),('loaded',C.c_int),('total',C.c_uint32),('free',C.c_uint32)]
assert C.sizeof(Entry)==144 and C.sizeof(Pane)==74008
def panes(q):return (Pane*2).from_buffer_copy(q.mem('panes',C.sizeof(Pane)*2))
def until(test,label):
    end=time.monotonic()+10
    while not test():assert time.monotonic()<end,label;time.sleep(.025)
def gui(q):q.text(('vidmode 640 480\n' if SMALL else '')+'guimenu\n');until(lambda:q.num('running')==1,'GUI start')
def command(q,cmd,message):
    offset=len(q.log.read_text(errors='replace'));q.text(cmd+'\n');until(lambda:message in q.log.read_text(errors='replace')[offset:],cmd);return q.log.read_text(errors='replace')[offset:]
def digest():return hashlib.sha256(image.read_bytes()).hexdigest()
def capture(q,name):(smoke.WORK/name).write_text(q.log.read_text(errors='replace'))
with smoke.Guest() as q:
    before=digest();command(q,'vls /mnt/kv','0 entries, 0 directories, 0 bytes')
    for path in ('/mnt/missing','/mnt/ram/readme.txt'):
        output=command(q,'vls '+path,'Listing unavailable or read error; output incomplete');assert '0 entries' not in output
    gui(q);q.open_app(2);assert all(p.loaded and not p.count and p.total for p in panes(q))
    assert not any(h[4]>=1000 for h in q.hits(2)) and digest()==before
    q.close_app(2);q.exit_gui();capture(q,'empty.serial.log')
image=smoke.WORK/'fenced.img';k=KvFS(str(image),16);k.add('old.txt',b'PRESERVE',False)
config=smoke.WORK/'metadata.conf';config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\nimmediately = "on"\nsector = "'+str(DIR_SECTOR)+'"\n');smoke.DISK=f'blkdebug:{config}:{image}'
with smoke.Guest() as q:
    before=digest();gui(q);q.open_app(2);q.widget(712,2);q.widget(717,2);assert not q.num('mounted')
    q.widget(714,2);assert all(not p.loaded and not p.count and not p.total and not p.free for p in panes(q))
    assert struct.unpack('<II',q.mem('pane_namespace',8))==(0,0) and not any(h[4]>=1000 for h in q.hits(2)) and digest()==before
    q.close_app(2);q.exit_gui();output=command(q,'vls /mnt/kv','Listing unavailable or read error; output incomplete');assert '0 entries' not in output
    command(q,'vls /mnt/ram',f'{len(q.files())} entries,');assert digest()==before
    gui(q);q.open_app(8);q.widget(201,8);assert q.num('mounted')==1 and digest()==before
    q.close_app(8);q.open_app(2);q.widget(714,2);assert all(p.loaded and p.count==1 and p.total for p in panes(q))
    q.close_app(2);q.exit_gui();command(q,'vls /mnt/kv','1 entries, 0 directories, 8 bytes');capture(q,'fence-check.serial.log')
smoke.DISK=image
with smoke.Guest() as q:assert q.num('mounted')==1 and {e['name'] for e in KvFS(str(image)).listing()}=={'old.txt'}
result={'arch64':smoke.IS64,'small_640x480':SMALL,'checks':'formatted empty root remains valid empty, vls clean zero count; unknown mount/file path is error not empty; real metadata EIO, explicit Refresh keeps unavailable panes unloaded/no rows/zero space; vls error not false zero count and RAM still lists; explicit read-only Check then Refresh recovers one file without writes, cold persistence','scope':'FAT read cuts and synthetic NTFS resident/extension I/O host-only; no physical media/hotplug/parser-completeness qualification'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2));print('PASS truthful listing','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
