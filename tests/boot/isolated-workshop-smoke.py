#!/usr/bin/env python3
"""Native in-OS MCC -> ABI4 via mouse opt-in, not host-built app injection."""
from pathlib import Path
import os,sys,subprocess,time,struct,json
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
from isolated_source import SOURCE,EXPECTED,RESOURCE_SOURCES,REUSED_NATIVE_FAULTS
assert all(max(map(len,s.splitlines()))<=120 for s in [SOURCE,*RESOURCE_SOURCES.values(),*(v[0] for v in REUSED_NATIVE_FAULTS.values())])
smoke.WORK=ROOT/'build/isolated-workshop'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'app.img'
subprocess.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=subprocess.DEVNULL)
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS
disk=KvFS(str(smoke.DISK));disk.add('verify.c',SOURCE,False)
for name,source in RESOURCE_SOURCES.items():disk.add(name,source,False)
def until(test,what,timeout=12):
    end=time.monotonic()+timeout
    while not test():
        assert time.monotonic()<end,what
        time.sleep(.04)
with smoke.Guest() as q:
    q.text('guimenu\n');time.sleep(.6);q.open_app(9)
    assert q.num('dev_isolated')==0,'legacy default changed'
    baseline=q.num('used_frames');q.widget(110,9);assert q.num('dev_isolated')==1
    q.widget(91,9)
    until(lambda:q.num('loaded')==1,'ABI4 template startup: '+repr(q.mem('last_error',96)))
    assert q.num('isolated')==1 and q.num('app_resident')==1
    blob=q.files()['hello.kapp'];assert struct.unpack_from('<HHI',blob,4)==(2,96,4)
    (smoke.WORK/'guest-hello.kapp').write_bytes(blob)
    def user_num(off):
        p=smoke.symbols['process']+(64 if smoke.IS64 else 48)+(off//4096)*4
        return q.num(q.num(p)+off%4096)
    count=0x8000+(72 if smoke.IS64 else 40)+len('hello.c')+1
    assert user_num(count)==0
    w=q.window(10);q.click(w['x']+40,w['y']+100)
    until(lambda:user_num(count)==1,'private native C click callback')
    time.sleep(.2);q.cmd(f'screendump "{smoke.WORK / "template.ppm"}"')
    ppm=(smoke.WORK/'template.ppm').read_bytes();assert ppm.count(bytes.fromhex('3ca0f0'))>=1500,'user runtime did not draw square'
    q.close_app(10);until(lambda:q.num('app_resident')==0,'template teardown')
    assert q.num('used_frames')==baseline
    # Open SOURCE only from a disposable KvFS fixture; MCC inside OS builds it.
    q.open_app(9);q.widget(41,9);q.choose_name('verify.c');q.widget(82,9);until(lambda:q.mode(9)==0,'source fixture did not load');q.widget(91,9)
    until(lambda:q.num('loaded')==1,'ABI4 advanced source startup: '+repr(q.mem('last_error',96)))
    assert q.num('isolated')==1 and q.num('app_faulted')==0
    service=smoke.symbols['services'];status=service+(13636 if smoke.IS64 else 13616)
    until(lambda:q.mem(status,96).split(b'\0')[0]==EXPECTED,'user-only heap/format/arrays/structs/private-file proof')
    word=8 if smoke.IS64 else 4
    clock_off=0x8000+(((72 if smoke.IS64 else 40)+len('verify.c')+word)&~(word-1))
    user_clock=[user_num(clock_off+4*i) for i in range(3)]
    trusted_clock=struct.unpack('<III',q.mem('cached_hms',12))
    to_seconds=lambda v:v[0]*3600+v[1]*60+v[2]
    assert (to_seconds(trusted_clock)-to_seconds(user_clock))%86400<=5,(user_clock,trusted_clock)
    files=service+(108 if smoke.IS64 else 96)
    assert q.mem(files,32).split(b'\0')[0]==b'local.txt'
    assert q.mem(files+44,len(EXPECTED)+1)==EXPECTED+b'\0'
    good=q.files()['verify.kapp'];assert struct.unpack_from('<I',good,8)[0]==4
    (smoke.WORK/'guest-verify.kapp').write_bytes(good)
    q.cmd(f'screendump "{smoke.WORK / "advanced.ppm"}"')
    assert (smoke.WORK/'advanced.ppm').read_bytes().count(bytes.fromhex('fc6a83'))>=100,'clipped user LINE missing'
    q.close_app(10);assert q.num('used_frames')==baseline
    q.open_app(9);q.key('ctrl-home');q.text('void bad(void){beep(440,10);}\n');q.widget(91,9)
    assert q.num('loaded')==0 and q.num('app_resident')==0,'unsupported API ran stale binary'
    assert q.files()['verify.kapp']==good,'failed build changed published image'
    assert q.mem('error_text',8 if smoke.IS64 else 4)!=b'\0'*(8 if smoke.IS64 else 4)
    assert 'local.txt' not in {e['name'] for e in KvFS(str(smoke.DISK)).listing()},'ABI4 file escaped before legacy launch'
    def launch_source(name,replacement=None):
        q.open_app(9);q.widget(41,9)
        if q.mode(9)==3:q.widget(84,9) # discard our deliberate unsupported-API edit
        q.choose_name(name);q.widget(82,9);until(lambda:q.mode(9)==0,'cannot open '+name)
        if replacement is not None:q.widget(81,9);q.text(replacement.decode())
        q.widget(91,9)
    faults={}
    for name,diagnostic in [('kernel.c',b'reason 2, vector 14'),('quota.c',b'value 31'),('textq.c',b'value 32'),('spin.c',b'reason 5')]:
        launch_source(name)
        until(lambda:q.num('app_resident')==0 and q.num('loaded')==0,'resource fixture not stopped: '+name)
        message=q.mem('last_error',96).split(b'\0')[0]
        assert diagnostic in message,(name,message)
        assert q.num('used_frames')==baseline,'resource fixture leaked pages'
        faults[name]=message.decode();q.close_app(10)
    for name,(source,diagnostic) in REUSED_NATIVE_FAULTS.items():
        launch_source('quota.c',source)
        until(lambda:q.num('app_resident')==0 and q.num('loaded')==0,'reused negative fixture not stopped: '+name)
        message=q.mem('last_error',96).split(b'\0')[0]
        assert diagnostic in message,(name,message)
        assert q.num('used_frames')==baseline,'reused negative fixture leaked pages: '+name
        faults[name]=message.decode();q.close_app(10)
    for name,expected in [('heap.c',b'heap bounded'),('files.c',b'files bounded')]:
        launch_source(name);until(lambda:q.num('loaded')==1,'limit-check startup')
        until(lambda:q.mem(status,96).split(b'\0')[0]==expected,'limit-check '+name)
        q.close_app(10);assert q.num('used_frames')==baseline
    # Reuse existing file slots: the desktop RAMFS is bounded to 32 entries.
    # A simple legacy source has no file operations, so it cannot hide a leak.
    assert not ({'f0','f1','f2','f3','f4','f5','f6','f7','ninth'} & {e['name'] for e in KvFS(str(smoke.DISK)).listing()})
    q.open_app(9);q.widget(81,9);q.text('#include <kvui.h>\nvoid draw(void){kv_point p;p.x=KV_CHAR_W;clear(p.x);}')
    q.widget(110,9)
    assert q.num('dev_isolated')==0;q.widget(91,9)
    until(lambda:q.num('loaded')==1,'legacy target relaunch');assert q.num('isolated')==0
    assert struct.unpack_from('<I',q.files()['files.kapp'],8)[0]==3
    q.close_app(10);q.exit_gui();assert q.num('used_frames')==baseline
fs=KvFS(str(smoke.DISK));names={v['name'] for v in fs.listing()}
assert 'local.txt' not in names,'isolated private file escaped into global KvFS'
(smoke.WORK/'result.json').write_text(json.dumps({'resource_faults':faults,'resource_limits':'32KiB heap exhaust/reuse; 8 private files / 1024-byte quota PASS','arch64':smoke.IS64,'compiler':'in-OS MCC, mouse ABI4 opt-in; ABI3 default preserved','proof':EXPECTED.decode(),'clock':{'user':user_clock,'trusted':trusted_clock},'checks':'native private draw/click; readonly virtual headers (nested/include-once), arrays/sizeof/macros/struct layout/8args; readonly cached RTC clock/null/alias outputs; user heap/format/private files; unsupported API leaves good output intact; legacy header compilation/relaunch; PMM baseline','global_private_file_absent':True},indent=2))
print('PASS isolated Workshop','UEFI64' if smoke.IS64 else 'BIOS32',': in-OS source compile, native callbacks/heap/format/private RAM, kernel-write/command/text/status/line/clock-pointer/spin termination, bounded heap/files and legacy switch',flush=True)
