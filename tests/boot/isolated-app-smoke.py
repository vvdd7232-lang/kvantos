#!/usr/bin/env python3
"""Opt-in format2 SDK app through the real Program Manager; private test disk only.
Not a host-import feature/workflow. No arbitrary file or raw-media grants to apps.
"""
from pathlib import Path
import os,sys,subprocess,time,hashlib,struct,json
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
smoke.WORK=ROOT/'build/isolated-app'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'app.img'
subprocess.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=subprocess.DEVNULL)
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS
sdk=ROOT/'sdk/isolated/build'/('boxes64.kapp' if smoke.IS64 else 'boxes32.kapp')
good=sdk.read_bytes();entry=struct.unpack_from('<I',good,20)[0]-0xe00000+96
fixtures=[('isobox.kapp',good)]
bad=bytearray(good);struct.pack_into('<I',bad,12,1 if smoke.IS64 else 257);fixtures.append(('badiso.kapp',bytes(bad)))
for name,code in [('fpuiso.kapp',b'\xd9\xe8'),('spiniso.kapp',b'\xeb\xfe'),('writeiso.kapp',b'\xb8\x00\x00\x10\x00\xc7\x00\x78\x56\x34\x12')]:
    blob=bytearray(good);blob[entry:entry+len(code)]=code;fixtures.append((name,bytes(blob)))
legacy=(ROOT/'release'/('apps64' if smoke.IS64 else 'apps')/'calc.kapp').read_bytes()
abi2=bytearray(legacy);struct.pack_into('<I',abi2,8,2);fixtures.append(('legacy2.kapp',bytes(abi2)))
badold=bytearray(legacy);struct.pack_into('<H',badold,6,65535);struct.pack_into('<I',badold,28,0xffffffff);fixtures.append(('badold.kapp',bytes(badold)))
fs=KvFS(str(smoke.DISK))
for name,data in fixtures:fs.add(name,data,True)
before=hashlib.sha256(smoke.DISK.read_bytes()).hexdigest()
def until(test,desc,timeout=10):
    end=time.monotonic()+timeout
    while not test():
        assert time.monotonic()<end,desc
        time.sleep(.03)
with smoke.Guest() as q:
    q.text('guimenu\n');time.sleep(.5);q.open_app(8);time.sleep(.2)
    baseline=q.num('used_frames')
    def launch(i):
        q.open_app(8);q.widget(210+i,8);time.sleep(.35)
    def closed():
        if any(w['app']==10 for w in q.windows()):q.close_app(10)
        until(lambda:q.num('app_resident')==0,'isolated teardown')
        assert q.num('used_frames')==baseline,'private frame leak'
    launch(0)
    until(lambda:q.num('app_resident')==1 and q.num('loaded')==1,'isolated startup')
    time.sleep(.4)
    assert q.num('isolated')==1 and q.num('app_faulted')==0
    assert q.mem('app_file',40).split(b'\0')[0]==b'isobox.kapp'
    assert b'ABI4' in q.mem('app_status',96)
    # Check the sandbox's file, not global RAM/KvFS. The SDK verified readback=42.
    service=smoke.symbols['services'];files=service+(108 if smoke.IS64 else 96)
    assert q.mem(files,32).split(b'\0')[0]==b'private.txt'
    assert q.num(files+32)==4 and q.num(files+44)==42
    symbols={}
    for line in subprocess.check_output(['nm','-n',str(sdk.with_suffix('.elf'))],text=True).splitlines():
        parts=line.split()
        if len(parts)==3:symbols[parts[2]]=int(parts[0],16)
    def user_num(name):
        off=symbols[name]-0xe00000
        frame=q.num(smoke.symbols['process']+(64 if smoke.IS64 else 48)+(off//4096)*4)
        return q.num(frame+off%4096)
    assert user_num('info_hz')==q.num('frequency') and user_num('info_lang')==q.num('kv_current_lang')
    assert user_num('status_result')==0
    old=user_num('clicks');w=q.window(10);q.click(w['x']+40,w['y']+100)
    until(lambda:user_num('clicks')>old,'isolated click delivery')
    yields=q.num('app_total_yields');q.key('p')
    until(lambda:user_num('delayed_draws')>0,'DRAW did not finish after quantum suspension')
    assert q.num('app_total_yields')>yields,'delayed DRAW did not actually yield'
    # Cover/uncover must replay the last validated canvas, not paint from int80.
    q.open_app(8);q.open_app(10);time.sleep(.2)
    q.cmd(f'screendump "{smoke.WORK / "isolated.ppm"}"');ppm=(smoke.WORK/'isolated.ppm').read_bytes()
    header,widthheight,depth,pixels=ppm.split(b'\n',3)
    width,height=map(int,widthheight.split());assert header==b'P6' and depth==b'255'
    green=[i//3 for i in range(0,len(pixels),3) if pixels[i:i+3]==bytes.fromhex('32c78a')]
    assert len(green)>100,'CPL3 draw commands did not reach client canvas'
    w=q.window(10)
    assert all(w['x']<i%width<w['x']+w['w']-1 and w['y']+22<i//width<w['y']+w['h']-20 for i in green),'draw escaped client'
    ink=[i//3 for i in range(0,len(pixels),3) if pixels[i:i+3]==bytes.fromhex('f1d257')]
    assert len(ink)>20 and user_num('text_result')>0,'bounded UTF-8 TEXT did not reach canvas'
    assert all(w['x']<i%width<w['x']+w['w']-1 and w['y']+22<i//width<w['y']+w['h']-20 for i in ink),'text escaped client'
    proof={'hz':user_num('info_hz'),'language':user_num('info_lang'),'status_result':user_num('status_result'),'text_bytes':user_num('text_result'),'text_pixels':len(ink),'draws':user_num('draws'),'draw_result':user_num('draw_result'),'delayed_draws':user_num('delayed_draws'),'yields':q.num('app_total_yields'),'green_pixels':len(green)}
    closed()
    for i in (1,2,3,4,6):
        launch(i)
        until(lambda:q.num('app_resident')==0 and q.num('loaded')==0,'fault/rejection did not unload',15)
        assert q.mem('last_error',96).split(b'\0')[0],fixtures[i][0]
        closed()
    launch(5);assert q.num('loaded')==1 and q.num('isolated')==0 and q.num('app_faulted')==0
    assert q.num('api_table')==2,'ABI2 prefix compatibility'
    closed()
    # Repeated ABI4 -> close -> ABI2 -> close must not retain grants/pages.
    for i in (0,5,0):
        launch(i);until(lambda:q.num('loaded')==1,'dual-loader relaunch');closed()
    q.open_app(7);q.text('desktop alive');assert q.document(7)['text']==b'desktop alive'
    q.close_app(7,discard=True);launch(0);until(lambda:q.num('app_resident')==1,'GUI-exit app startup')
    q.exit_gui();assert q.num('app_resident')==0 and q.num('loaded')==0,'GUI exit retained private grants/pages'
    assert q.num('used_frames')==baseline,'GUI exit leaked private frames'
    result={'runtime_proof':proof,'arch64':smoke.IS64,'isolated_sdk':'PASS draw/UTF-8 text/click/private files, quantum-delayed draw replay, cover/uncover and GUI exit','faults':'wrong arch/FPU/spin/kernel write/legacy size-bss','dual_loader':'ABI2/ABI4 repeated PASS','disk_sha256':before,'scope':'experimental ABI4, SDK opt-in; Workshop default ABI3 remains ring0'}
assert hashlib.sha256(smoke.DISK.read_bytes()).hexdigest()==before,'isolated app modified global KvFS fixture'
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2))
print('PASS isolated SDK GUI','UEFI64' if smoke.IS64 else 'BIOS32',': draw/click/private RAM, malicious fixtures, slice budget, close/dual-loader/frame cleanup, source disk unchanged',flush=True)
