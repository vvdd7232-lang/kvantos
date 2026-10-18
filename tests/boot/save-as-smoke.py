#!/usr/bin/env python3
"""Mouse Save As -> VFS, independent targets, errors and cold readback.
Normal mode: RAM/KvFS + RO USB. GUI_SAVE_LAB=1: marked FAT/WAL + real unplug.
All devices are private regular-file fixtures; never mount or open host drives.
"""
from pathlib import Path
import ctypes as C
import hashlib, os, struct, subprocess as sp, sys, time, zlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        sp.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
import usb_topology as topology
from text_model import Doc,Undo,editor_type
from mkusb import make,make_gpt
LAB=os.environ.get('GUI_SAVE_LAB')=='1'
OWNED=os.environ.get('GUI_SAVE_OWNED')=='1'
assert not (LAB and OWNED)
FAT=LAB or OWNED
# A synchronous 64 KiB UHCI file write can exceed the normal input-only budget.
# Still ONE measured click; wait for completion, never retry the press.
smoke.Guest.click_timeout=60*(topology.DEPTH+1) if FAT else 8
assert ('fat_lab_stage' in smoke.symbols)==LAB,'build mode does not match GUI_SAVE_LAB'
CONTROLLER=os.environ.get('STORAGE_CONTROLLER',os.environ.get('USB_CONTROLLER','ehci-ohci'));LAYOUT=os.environ.get('STORAGE_LAYOUT',os.environ.get('USB_LAYOUT','gpt'))
assert CONTROLLER in ('uhci','ohci','ehci-ohci','ahci') and LAYOUT in ('mbr','gpt')
assert CONTROLLER!='ahci' or OWNED,'AHCI GUI qualification uses normal owned WAL only'
smoke.WORK=ROOT/'build/save-as'/('owned' if OWNED else 'lab' if LAB else 'normal')/CONTROLLER/LAYOUT/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK=topology.work(smoke.WORK)
smoke.WORK.mkdir(parents=True,exist_ok=True);smoke.DISK=smoke.WORK/'local.img'
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS
large=(b'a'*63+b'\n')*1023+b'z'*63
assert len(large)==65535
draft=(b'new revision\n'*100)
sp.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=sp.DEVNULL)
k=KvFS(str(smoke.DISK));k.add('existing.txt',b'old',False);k.add('huge.txt',large,False);k.add('draft.txt',draft,False)
if OWNED:
    from mkfatowned import make as make_owned
    source,first,total,journal,bpb=make_owned(smoke.WORK,LAYOUT)
elif LAB:
    from mkfatlab import make as make_lab
    source,first,total,journal,bpb=make_lab(smoke.WORK,LAYOUT)
else:
    raw,mbr,_=make(smoke.WORK);source=make_gpt(raw,smoke.WORK/'flash-gpt.img') if LAYOUT=='gpt' else mbr
vol=('/mnt/sata0p' if CONTROLLER=='ahci' else '/mnt/usb0p')+('2' if LAYOUT=='gpt' else '1')
Editor=editor_type(smoke.IS64)
def state(q,app=7):return Editor.from_buffer_copy(q.mem('dev_editor' if app==9 else 'note_editor',C.sizeof(Editor)))
def snap(q,app=7):return q.mem('dev_editor' if app==9 else 'note_editor',C.sizeof(Doc)+C.sizeof(Undo))
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def local(name):
    item=next((e for e in KvFS(str(smoke.DISK)).listing() if e['name']==name),None)
    if not item:return None
    with smoke.DISK.open('rb') as f:f.seek(item['start']*512);return f.read(item['size'])
def args(config=None):
    filename=f'blkdebug:{config or ""}:{source}' if (config or CONTROLLER=='ahci') else str(source)
    a=['-drive',f'if=none,id=flash,format=raw,file={filename}']
    if CONTROLLER=='ahci':a+=['-device','ich9-ahci,id=satatest']
    elif CONTROLLER=='ohci':a+=['-device','pci-ohci,id=usbtest']
    elif CONTROLLER=='uhci':a+=['-device','piix3-usb-uhci,id=usbtest']
    else:a+=['-device','ich9-usb-ehci1,id=usbtest','-device','pci-ohci,id=comp0,masterbus=usbtest.0,firstport=0','-device','pci-ohci,id=comp1,masterbus=usbtest.0,firstport=3']
    if OWNED:
        (smoke.WORK/'debug.sock').unlink(missing_ok=True)
        a+=['-gdb','unix:'+str(smoke.WORK/'debug.sock')+',server=on,wait=off']
    if CONTROLLER=='ahci':return a+['-device','ide-hd,id=stick,bus=satatest.2,drive=flash']
    return a+topology.hub_args(CONTROLLER)+['-device','usb-storage,id=stick,bus=usbtest.0,drive=flash,port='+topology.leaf_port('1' if CONTROLLER in ('uhci','ohci') else '3')]
def start(q):
    if OWNED:
        q.text('fatwrite '+vol.rsplit('/',1)[1]+' enable\n')
        deadline=time.monotonic()+60
        while b'RW enabled for this session' not in q.log.read_bytes():
            assert time.monotonic()<deadline,q.log.read_bytes()[-1500:]
            time.sleep(.05)
    q.text('lang ru\nguimenu\n')
    deadline=time.monotonic()+15
    while not (q.num('scr_h') and q.num('running')):
        assert time.monotonic()<deadline,'GUI initialization timeout'
        time.sleep(.05)
    q.open_app(7)
def picker(q,path,app=7):
    q.widget(76,app)
    if q.mode(app)==1:q.widget(99,app)
    assert q.mode(app)==9,(q.mode(app),state(q,app).status)
    for _ in range(9):
        if state(q,app).path.decode()==path:return
        q.widget(99,app)
    raise AssertionError(('volume not found',path,state(q,app).path))
def choose(q,name,app=7):
    s=state(q,app);names=[e.name.decode() for e in s.items[:s.count]]
    assert name in names,(name,names,s.path)
    widget=2000+names.index(name)
    for _ in range(128):
        if widget in {h[4] for h in q.hits(app)}:break
        q.widget(86,app)
    else:raise AssertionError('row never visible')
    q.widget(widget,app)
def accept(q,app=7):q.widget(82,app)
def open_local(q,name):
    q.widget(41,7)
    if q.mode()==3:q.widget(84,7)
    q.widget(95,7);q.choose_name(name);accept(q)
    assert q.mode()==0,(q.mode(),state(q).status)
def saved(q,path,app=7):
    s=state(q,app)
    assert s.target.decode()==path and s.target_valid and not s.target_failed,(s.target,s.status)
    assert s.target_revision==s.undo.state[4] and not q.document(app)['dirty']
print('Save As:', 'normal owned FAT/WAL' if OWNED else 'LAB FAT/WAL' if LAB else 'normal RAM/KvFS/RO',CONTROLLER,LAYOUT,'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
original=digest(source)
if not FAT:
    with smoke.Guest(extra_args=args()) as q:
        start(q);q.text('selected');picker(q,'/mnt/kv');q.key('alt-shift');q.choose_name('only.txt');q.key('alt-shift');accept(q)
        saved(q,'/mnt/kv/only.txt');assert local('only.txt')==b'selected' and 'only.txt' not in q.files()
        q.text('!');q.widget(42,7);saved(q,'/mnt/kv/only.txt');assert local('only.txt')==b'selected!'
        q.widget(77,7);assert q.document()['text']==b'selected' and q.document()['dirty']
        q.widget(100,7);assert not q.document()['dirty']
        picker(q,'/mnt/kv');q.choose_name('existing.txt');before=snap(q);accept(q)
        assert q.mode()==10 and local('existing.txt')==b'old'
        q.widget(83,7);assert q.mode()==9 and snap(q)==before
        accept(q);accept(q);saved(q,'/mnt/kv/existing.txt');assert local('existing.txt')==b'selected!'
        assert 'existing.txt' not in q.files()
        picker(q,'/mnt/ram');q.choose_name('shared.c');accept(q);saved(q,'/mnt/ram/shared.c')
        assert q.files()['shared.c']==b'selected!' and local('shared.c') is None
        q.text('2');q.widget(42,7);assert q.files()['shared.c']==b'selected!2' and local('shared.c') is None
        q.key('ctrl-a');q.key('backspace');q.widget(42,7)
        saved(q,'/mnt/ram/shared.c');assert q.files()['shared.c']==b''
        q.widget(77,7);assert q.document()['dirty'];q.widget(100,7);assert not q.document()['dirty']
        alias=state(q).name;old_disk_valid=state(q).disk_valid
        long_name='long-destination-name-1234567890.txt'
        picker(q,'/mnt/kv');q.choose_name(long_name);accept(q)
        saved(q,'/mnt/kv/'+long_name)
        assert state(q).name==alias and state(q).disk_valid==old_disk_valid and local(long_name)==b''
        picker(q,'/mnt/ram');q.choose_name('shared.c');accept(q);accept(q)
        print('  selected-only KvFS/RAM, bound Save, replace/cancel, Undo/Redo checkpoint PASS',flush=True)
        q.open_app(9);template=q.document(9)['text'];picker(q,'/mnt/kv',9);q.choose_name('chosen.c');accept(q,9)
        saved(q,'/mnt/kv/chosen.c',9);assert local('chosen.c')==template and 'chosen.c' not in q.files()
        q.widget(90,9);assert q.files()['chosen.kapp'][:4]==b'KAPP'
        picker(q,'/mnt/kv',9);q.choose_name('UPPER.C');accept(q,9)
        assert state(q,9).name==b'chosen.c' and local('UPPER.C')==template
        q.widget(90,9);assert q.files()['chosen.kapp'][:4]==b'KAPP'
        picker(q,'/mnt/ram',9);q.choose_name('shared.c');accept(q,9);assert q.mode(9)==10;accept(q,9)
        assert not state(q,7).target_valid and q.document(7)['dirty']
        q.open_app(7);before=snap(q);files=q.files();old_target=state(q).target
        picker(q,vol);q.choose_name('must-not.txt');accept(q)
        assert q.mode()==9 and b'RO' in state(q).status and snap(q)==before
        assert state(q).target==old_target and q.files()==files
        q.screenshot(smoke.WORK/'ro-destination.png');q.widget(83,7)
        print('  workshop Build, independent destinations, cross-editor invalidation, RO failure atomic PASS',flush=True)
        picker(q,'/mnt/ram');q.widget(97,7);q.widget(83,7)
        before=snap(q);files=q.files();q.widget(42,7)
        assert snap(q)==before and q.files()==files and state(q).target_failed
        print('  namespace rescan cannot silently rebind old destination PASS',flush=True)
    assert digest(source)==original,'ordinary USB was modified'
    with smoke.Guest(extra_args=args()) as q:
        start(q);open_local(q,'only.txt');assert q.document()['text']==b'selected!'
        assert not state(q).target
        print('  fresh VM reopens selected KvFS file; old binding cleared PASS',flush=True)
        if not smoke.IS64:
            q.exit_gui();q.text('setup --yes --keep\n')
            deadline=time.monotonic()+20
            while q.num('setup_percent')!=100:
                assert time.monotonic()<deadline,'installer did not finish'
                time.sleep(.05)
            q.cmd('boot_set c');q.cmd('eject -f ide1-cd0')
            prompts=q.log.read_bytes().count(b'kvant:/');q.cmd('system_reset')
            deadline=time.monotonic()+45
            while q.log.read_bytes().count(b'kvant:/')<=prompts:
                assert time.monotonic()<deadline,'installed gzip-kernel BIOS boot timed out'
                time.sleep(.1)
            start(q);open_local(q,'only.txt');assert q.document()['text']==b'selected!'
            picker(q,'/mnt/kv');q.widget(83,7)
            print('  current compressed standalone GRUB: CD ejected, full reset, saved file + Save As reopen PASS',flush=True)
    with smoke.Guest(disk=False,extra_args=args()) as q:
        q.text('vidmode 640 480\n');start(q);q.text('temporary');picker(q,'/mnt/ram');q.choose_name('small.txt');accept(q)
        saved(q,'/mnt/ram/small.txt');assert q.files()['small.txt']==b'temporary'
        picker(q,vol);q.choose_name('no.txt');before=snap(q);accept(q)
        assert q.mode()==9 and snap(q)==before and b'RO' in state(q).status
        q.screenshot(smoke.WORK/'save-as-640x480.png')
        print('  diskless RAM-only and RO destination at 640x480 PASS',flush=True)
    assert digest(source)==original
    # Explicit VFS failure must not silently fall back to saving a RAM copy.
    k=KvFS(str(smoke.DISK))
    for i in range(64-len(k.listing())):k.add(f'full{i:02d}.txt',b'x',False)
    with smoke.Guest(extra_args=args()) as q:
        start(q);q.text('unsaved');picker(q,'/mnt/kv');q.choose_name('no-room.txt')
        before=snap(q);files=q.files();disk_before=digest(smoke.DISK);accept(q)
        assert q.mode()==9 and snap(q)==before and q.files()==files and not state(q).target
        assert local('no-room.txt') is None and q.document()['dirty']
        assert b'WRITE FAILED' in state(q).status or 'ОШИБКА'.encode() in state(q).status
        print('  selected KvFS full: no false success, no RAM fallback, document/Undo intact PASS',flush=True)
    assert digest(smoke.DISK)==disk_before and digest(source)==original
    raise SystemExit
# FAT mode: compare the whole namespace/content and every byte outside FAT+WAL.
size=source.stat().st_size
chunks=[]
with source.open('rb') as f:
    while b:=f.read(65536):
        if b.strip(b'\0'):chunks.append((f.tell()-len(b),b))
def reset():
    with source.open('wb') as f:
        f.truncate(size)
        for at,b in chunks:f.seek(at);f.write(b)
def span(a,b):
    h=hashlib.sha256()
    with source.open('rb') as f:
        f.seek(a)
        while a<b:
            data=f.read(min(65536,b-a));assert data;h.update(data);a+=len(data)
    return h.digest()
ranges=([(0,first*512),((first+total)*512,size),((journal-1)*512,journal*512)] if OWNED else
        [(0,first*512),((first+total)*512,journal*512),((journal+49)*512,size)])
outside=[span(a,b) for a,b in ranges]
def external():
    assert [span(a,b) for a,b in ranges]==outside,'write escaped FAT/WAL'
    part=smoke.WORK/'check.img'
    with source.open('rb') as src,part.open('wb') as dst:
        src.seek(first*512);dst.truncate(total*512)
        for at in range(0,total*512,65536):
            data=src.read(min(65536,total*512-at))
            if data.strip(b'\0'):dst.seek(at);dst.write(data)
    result=sp.run(['/sbin/fsck.fat','-n',str(part)],capture_output=True,text=True)
    assert result.returncode==0,(result.stdout,result.stderr)
    import shutil
    out=smoke.WORK/'export'
    if out.exists():shutil.rmtree(out)
    out.mkdir();sp.run(['mcopy','-s','-i',str(part),'::*',str(out)],check=True,capture_output=True)
    return {p.relative_to(out).as_posix():(None if p.is_dir() else p.read_bytes()) for p in out.rglob('*') if p.name.upper()!='KVANTWAL.SYS'}
old=external()
with smoke.Guest(extra_args=args()) as q:
    start(q);open_local(q,'huge.txt');assert q.document()['text']==large and q.document()['lines']>128
    picker(q,vol);choose(q,'Empty long directory');q.choose_name('Large.txt')
    began=time.monotonic();accept(q)
    print(f'  MEASURE GUI Save As 65535 bytes: {time.monotonic()-began:.3f}s (single click, unchanged deadline)',flush=True)
    saved(q,vol+'/Empty long directory/Large.txt')
    assert local('huge.txt')==large and 'Large.txt' not in q.files()
    q.key('ctrl-a');q.text('edited');q.widget(42,7);saved(q,vol+'/Empty long directory/Large.txt')
    for _ in range(6):q.widget(77,7)
    assert q.document()['text']==large and q.document()['dirty']
    for _ in range(6):q.widget(100,7)
    assert q.document()['text']==b'edited' and not q.document()['dirty']
    picker(q,vol+'/Empty long directory');q.widget(98,7);choose(q,'Old long name.txt');accept(q)
    assert q.mode()==10;q.widget(83,7);assert q.mode()==9;accept(q);accept(q)
    saved(q,vol+'/Old long name.txt')
    picker(q,vol);q.key('ctrl-u');q.key('alt-shift');q.text('ntcn');q.key('alt-shift');q.text('.txt')
    assert state(q).save_name=='тест.txt'.encode(),state(q).save_name
    accept(q);saved(q,vol+'/тест.txt')
    picker(q,vol);q.choose_name('Empty long directory');before=snap(q);accept(q)
    assert q.mode()==9 and snap(q)==before;q.widget(83,7)
    picker(q,vol);q.choose_name('Shared.c');accept(q)
    q.open_app(9);template=q.document(9)['text'];picker(q,vol,9);q.choose_name('SHARED.C');accept(q,9)
    assert q.mode(9)==10;accept(q,9)
    assert not state(q,7).target_valid and q.document(7)['dirty'],'FAT case alias left stale saved marker'
    q.open_app(7);q.screenshot(smoke.WORK/'fat-saved.png')
expected=dict(old);expected.update({'Empty long directory/Large.txt':b'edited','Old long name.txt':b'edited','тест.txt':b'edited','Shared.c':template})
assert external()==expected
with smoke.Guest(extra_args=args()) as q:
    start(q);q.widget(96,7);choose(q,'тест.txt');q.choose_name('roundtrip.txt');accept(q)
    assert q.document()['text']==b'edited' and q.files()['roundtrip.txt']==b'edited'
assert external()==expected
print('  GUI -> FAT folder: 65535 bytes, bound Save, LFN UTF-8, replace/cancel, directory refusal, cold GUI import/fsck PASS',flush=True)
if OWNED:
    # No lab hooks: stop the NORMAL kernel at commit entry via QEMU's debugger.
    # Metadata has been published, but the UI has not received success and the
    # WAL has not closed. An actual device_del must fail, retaining the document.
    from qemu_break import Breakpoint
    reset()
    with smoke.Guest(extra_args=args()) as q:
        start(q);open_local(q,'draft.txt');q.text('!')
        picker(q,vol);choose(q,'Old long name.txt');accept(q);assert q.mode()==10
        before=snap(q);files=q.files();target=state(q).target
        intended=q.document()['text']
        bp=Breakpoint(q,smoke.WORK/'debug.sock','stx_commit')
        x,y,w,h,_,_=next(h for h in q.hits(7) if h[4]==82)
        q.move(x+w//2,y+h//2);q.cmd('mouse_button 1');bp.wait()
        assert snap(q)==before,'uncommitted operation claimed saved checkpoint'
        if CONTROLLER=='ahci':
            with source.open('rb') as f:
                f.seek(journal*512);active=f.read(512)
            assert struct.unpack_from('<I',active,32)[0]==1,'breakpoint must precede WAL close'
            # QEMU AHCI's IDE bus rejects hot-unplug. Suspend the REAL host
            # flush instead: device timeout + late completion, no kernel patch.
            reply=q.cmd('qemu-io flash "break flush_to_disk guihang"')
        else:reply=q.cmd('device_del stick')
        assert b'Error' not in reply and b'not found' not in reply,reply
        bp.resume();deadline=time.monotonic()+20
        while q.mode()!=9:
            assert time.monotonic()<deadline,'USB failure exceeded time bound'
            time.sleep(.05)
        q.cmd('mouse_button 0')
        assert snap(q)==before and q.files()==files and state(q).target==target
        assert b'WRITE FAILED' in state(q).status or 'ОШИБКА'.encode() in state(q).status,state(q).status
        q.screenshot(smoke.WORK/('timeout-keeps-document.png' if CONTROLLER=='ahci' else 'unplug-keeps-document.png'))
        if CONTROLLER=='ahci':
            reply=q.cmd('qemu-io flash "resume guihang"');assert b'Error' not in reply,reply
            assert snap(q)==before and q.files()==files,'late DMA completion changed document/checkpoint'
    committed_late=False
    if CONTROLLER=='ahci':
        with source.open('rb') as f:
            f.seek(journal*512);header=f.read(512)
        assert zlib.crc32(header[:508])==struct.unpack_from('<I',header,508)[0]
        committed_late=struct.unpack_from('<I',header,32)[0]==0
        if committed_late:assert struct.unpack_from('<I',header,36)[0]==0
    with smoke.Guest(extra_args=args()) as q:
        recovered=b'FAT owned WAL recovered BEFORE namespace publication' in q.log.read_bytes()
        assert recovered!=committed_late
    after=dict(old)
    if committed_late:after['Old long name.txt']=intended
    assert external()==after
    if CONTROLLER=='ahci':
        print('  NORMAL AHCI flush timeout + late completion: no saved checkpoint, document/Undo kept; '+
              ('valid committed-new view' if committed_late else 'recovered-old view')+' + cold fsck PASS',flush=True)
    else:
        print('  NORMAL kernel, debugger commit cut + actual unplug: bounded failure, document/Undo kept, cold recovery/fsck PASS',flush=True)
    raise SystemExit
# Derive the GUI overwrite's directory cut from actual sector events, then detach
# during the same real operation. Only one measured mouse press is sent.
reset()
with smoke.Guest(extra_args=args()) as q:
    q.text('fattest arm 0\n');start(q);open_local(q,'draft.txt');q.text('!')
    picker(q,vol);choose(q,'Old long name.txt');accept(q);accept(q)
    n=q.num('stx_lab_step');assert n<=256
    trace=list(struct.iter_unpack('<II',q.mem('stx_lab_events',n*8)))
    root=first+struct.unpack_from('<H',bpb,14)[0]+2*struct.unpack_from('<I',bpb,36)[0]
    cut=next(i+1 for i,(phase,lba) in enumerate(trace) if phase==2 and lba==root)
reset()
with smoke.Guest(extra_args=args()) as q:
    q.text(f'fattest arm {-cut}\n');start(q);open_local(q,'draft.txt');q.text('!')
    picker(q,vol);choose(q,'Old long name.txt');accept(q);assert q.mode()==10
    before=snap(q);files=q.files();target=state(q).target
    x,y,w,h,_,_=next(h for h in q.hits(7) if h[4]==82)
    q.move(x+w//2,y+h//2);q.cmd('mouse_button 1')
    deadline=time.monotonic()+8
    while q.num('stx_lab_step')!=cut:
        assert time.monotonic()<deadline,'GUI never reached selected metadata cut'
        time.sleep(.02)
    assert snap(q)==before,'paused operation claimed saved checkpoint'
    reply=q.cmd('device_del stick');assert b'Error' not in reply,reply
    deadline=time.monotonic()+12
    while q.mode()!=9:
        assert time.monotonic()<deadline,'USB write did not fail within bound'
        time.sleep(.05)
    q.cmd('mouse_button 0')
    assert snap(q)==before and q.files()==files and state(q).target==target
    assert b'WRITE FAILED' in state(q).status or 'ОШИБКА'.encode() in state(q).status,state(q).status
    q.screenshot(smoke.WORK/'unplug-keeps-document.png')
with smoke.Guest(extra_args=args()) as q:
    q.text('fattest verify\n')
    deadline=time.monotonic()+5
    while b'M2FAT result=1000' not in q.log.read_bytes():
        assert time.monotonic()<deadline,q.log.read_bytes()[-1000:]
        time.sleep(.02)
assert external()==old
print('  actual unplug after GUI directory publication -> bounded error, document/Undo kept, cold recovery old file/fsck PASS',flush=True)
