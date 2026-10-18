#!/usr/bin/env python3
"""Real USB BOT/UHCI/EHCI + FAT32 + mouse import + native build/run, both kernels.
Only private fixture images are used. QEMU attaches them writable so the final
SHA-256 check can actually detect an accidental write by firmware or the OS.
"""
from pathlib import Path
import ctypes as C
import hashlib, os, struct, subprocess, sys, time
ROOT=Path(__file__).resolve().parents[2]
os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
import usb_topology as topology
from mkusb import make, make_gpt
CONTROLLER=os.environ.get('USB_CONTROLLER','uhci')
assert CONTROLLER in ('uhci','ohci','ehci','ehci-ohci')
EHCI=CONTROLLER.startswith('ehci')
PORT=topology.leaf_port('3' if CONTROLLER=='ehci-ohci' else '1')
LAYOUT=os.environ.get('USB_LAYOUT','mbr')
assert LAYOUT in ('mbr','gpt')
SOURCE_ROOT=b'/mnt/usb0p2' if LAYOUT=='gpt' else b'/mnt/usb0p1'
smoke.WORK=ROOT/'build/usb-import'/CONTROLLER/LAYOUT/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK=topology.work(smoke.WORK)
smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'local.img'
raw,mbr,proof=make(smoke.WORK)
source=make_gpt(raw,smoke.WORK/'flash-gpt.img') if LAYOUT=='gpt' else mbr
subprocess.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=subprocess.DEVNULL)
sys.path.insert(0,str(ROOT/'sdk'))
from mkdisk import KvFS

from text_model import Doc, Undo, editor_type
Editor=editor_type(smoke.IS64)
def state(q,app=9):return Editor.from_buffer_copy(q.mem('dev_editor' if app==9 else 'note_editor',C.sizeof(Editor)))
def snap(q,app=9):return q.mem('dev_editor' if app==9 else 'note_editor',C.sizeof(Doc)+C.sizeof(Undo))
def open_import(q,app=9):
    q.widget(96,app)
    if q.mode(app)==3:q.widget(84,app)
    assert q.mode(app)==6,(q.mode(app),state(q,app).status)
def choose(q,name,app=9):
    s=state(q,app)
    names=[bytes(e.name).decode('utf8') for e in s.items[:s.count]]
    assert name in names,(name,names,s.path)
    wid=2000+names.index(name)
    for _ in range(128):
        if wid in {h[4] for h in q.hits(app)}:break
        q.widget(86,app)
    else:raise AssertionError(('row never visible',name))
    q.widget(wid,app)
def refresh(q, present=True, app=9):
    q.widget(97,app)
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        s=state(q,app)
        if q.num('disk_count')==int(present) and bool(s.count)==present:
            if not present or s.path:return
        time.sleep(.05)
    raise AssertionError(('refresh did not finish',q.num('disk_count'),state(q,app).count,state(q,app).status))
def usb_args(image):
    model='ich9-usb-ehci1' if CONTROLLER=='ehci-ohci' else 'usb-ehci' if EHCI else 'pci-ohci' if CONTROLLER=='ohci' else 'piix3-usb-uhci'
    args=['-device',model+',id=usbtest']
    if CONTROLLER=='ehci-ohci':
        args+=['-device','pci-ohci,id=companion0,masterbus=usbtest.0,firstport=0,num-ports=3',
               '-device','pci-ohci,id=companion1,masterbus=usbtest.0,firstport=3,num-ports=3']
    return args+topology.hub_args(CONTROLLER)+['-drive',f'file={image},format=raw,if=none,id=flash',
                 '-device',f'usb-storage,bus=usbtest.0,drive=flash,id=stick,port={PORT}']
def local_read(name):
    entry=next(e for e in KvFS(str(smoke.DISK)).listing() if e['name']==name)
    with smoke.DISK.open('rb') as f:
        f.seek(entry['start']*512);return f.read(entry['size'])
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def imported(q,expected,name,app=9):
    assert q.mode(app)==0,(q.mode(app),state(q,app).status)
    assert q.document(app)['text']==expected,q.document(app)
    assert q.files()[name]==expected,(name,q.files().keys())

print('USB import:',CONTROLLER,LAYOUT, 'UEFI x86-64' if smoke.IS64 else 'BIOS i386',flush=True)
original={p:digest(p) for p in {raw,mbr,source}}
with smoke.Guest(extra_args=usb_args(source)) as q:
    expected_hcs=3 if CONTROLLER=='ehci-ohci' else 1  # EHCI plus two OHCI companions
    assert q.num('hc_count')==expected_hcs and q.num('disk_count')==1
    assert q.num('ehci_count' if EHCI else 'ohci_count' if CONTROLLER=='ohci' else 'uhci_count')==1
    assert (b'Speed 480 Mb/s' if EHCI else b'Speed 12 Mb/s') in q.cmd('info usb')
    assert SOURCE_ROOT.decode() in q.log.read_text(errors='replace')
    q.text('lang ru\n');q.text('guimenu\n');time.sleep(1)
    q.open_app(9);before=snap(q)
    # Dirty guard and cancel leave the entire document + undo unchanged.
    q.widget(96,9);assert q.mode(9)==3;q.widget(83,9);assert snap(q)==before
    open_import(q);assert state(q).path==SOURCE_ROOT
    choose(q,'Sources');assert state(q).path.endswith(b'/Sources')
    choose(q,'Nested');choose(q,'BOM.C');q.choose_name('proof.c')
    q.screenshot(smoke.WORK/'import-dialog-ru.png')
    q.widget(82,9);imported(q,proof,'proof.c')
    assert state(q).ram_valid and not state(q).disk_valid and state(q).imported
    q.widget(109,9);assert local_read('proof.c')==proof
    assert state(q).disk_valid and not state(q).imported and not q.document(9)['dirty']
    print('  Explicit Keep: KvFS after import (no automatic USB/disk write) PASS',flush=True)
    q.widget(90,9);assert q.files()['proof.kapp'][:4]==b'KAPP'
    q.widget(91,9);time.sleep(.4)
    assert q.num('loaded')==1 and q.num('app_faulted')==0
    assert q.mem('app_status',96).split(b'\0')[0]==b'USB import OK'
    q.screenshot(smoke.WORK/'imported-running.png')
    q.close_app(10);q.open_app(9)
    saved=local_read('proof.c');assert saved==proof
    print(f'  {LAYOUT.upper()} FAT32, folders, BOM/CRLF, rename, native Build/Run + KvFS save PASS',flush=True)
    # Every malformed input must preserve document AND undo AND RAM files.
    for name in ('BINARY.C','LARGE.C','WIDE.C','UTF16.C'):
        before=snap(q);files=q.files();open_import(q);choose(q,name);q.widget(82,9)
        assert q.mode(9)==6 and snap(q)==before and q.files()==files,(name,state(q).status)
        q.widget(83,9)
    print('  Binary, oversized, long line, UTF-16: atomic rejection PASS',flush=True)
    open_import(q);choose(q,'MULTI.C');q.choose_name('multi.c');q.widget(82,9)
    multi=(b'// '+b'x'*70+b'\n')*80+proof
    imported(q,multi,'multi.c');q.widget(90,9)
    assert q.files()['multi.kapp'][:4]==b'KAPP'
    print('  Multi-cluster 6 KiB source import + native Build PASS',flush=True)
    # 128 is now a warning threshold, not an import/edit/build ceiling.
    open_import(q);choose(q,'LINES.C');q.choose_name('lines.c');q.widget(82,9)
    imported(q,b'\n'*128,'lines.c');assert q.document(9)['lines']==129
    open_import(q);choose(q,'MANY.C');q.choose_name('many.c');q.widget(82,9)
    many=b'// line\n'*512+proof
    imported(q,many,'many.c');assert q.document(9)['lines']>512
    q.key('ctrl-a');q.key('right');q.key('ret')
    assert q.document(9)['text']==many+b'\n'
    q.widget(77,9);assert q.document(9)['text']==many
    q.widget(100,9);assert q.document(9)['text']==many+b'\n',q.document(9)
    q.widget(90,9);assert local_read('many.c')==many+b'\n'
    q.screenshot(smoke.WORK/'many-lines-warning.png')
    q.widget(91,9);time.sleep(.3)
    assert q.num('loaded')==1 and q.num('app_faulted')==0
    assert q.mem('app_status',96).split(b'\0')[0]==b'USB import OK'
    q.close_app(10);q.open_app(9)
    # A byte-limited compiler must not impose its limit on editing/saving.
    open_import(q);choose(q,'HUGE.C');q.choose_name('huge.c')
    began=time.monotonic();q.widget(82,9)
    print(f'  MEASURE GUI import HUGE.C: {time.monotonic()-began:.3f}s (single click, unchanged 8s deadline)',flush=True)
    huge=(b'// '+b'x'*70+b'\n')*250+proof
    imported(q,huge,'huge.c')
    q.key('ctrl-a');q.key('right');q.key('ret')
    huge+=b'\n';assert q.document(9)['text']==huge
    before=snap(q)
    q.widget(90,9)
    assert snap(q)==before and b'16000' in state(q).status
    assert 'huge.kapp' not in q.files()
    q.widget(42,9);assert local_read('huge.c')==huge
    print('  129/517+ lines: import/edit/undo/redo/save/native Build+Run; >16000 B edit/save, clear compiler error PASS',flush=True)
    # Empty file is valid. Long FAT name has a separate editable local name.
    open_import(q);choose(q,'EMPTY.C');q.widget(82,9);imported(q,b'','EMPTY.c')
    open_import(q);choose(q,'Sources');choose(q,'A long source name.C')
    q.choose_name('proof.c');before=snap(q);ram=q.files()['proof.c']
    q.widget(82,9);assert q.mode(9)==7;q.widget(83,9)
    assert q.mode(9)==6 and snap(q)==before and q.files()['proof.c']==ram
    q.widget(82,9);assert q.mode(9)==7;q.widget(82,9);imported(q,proof,'proof.c')
    open_import(q);choose(q,'OTHER.C');q.choose_name('proof.c')
    q.widget(82,9);assert q.mode(9)==7;q.widget(82,9)
    imported(q,proof.replace(b'42',b'77'),'proof.c')
    assert local_read('proof.c')==proof,'import wrote KvFS without Save'
    print('  Empty file, long name, overwrite confirmation/cancel and deferred disk save PASS',flush=True)
    # Independent Notepad, Unicode text and CRLF/tabs.
    q.open_app(7);q.text('private note');note=snap(q,7)
    open_import(q,7);q.widget(83,7);assert snap(q,7)==note
    open_import(q,7);choose(q,'NOTE.TXT',7);q.choose_name('note.txt');q.widget(82,7)
    imported(q,'Привет с флешки!\nUTF-8 и CRLF.\n'.encode(),'note.txt',7)
    open_import(q,7);choose(q,'TABS.TXT',7);q.widget(82,7)
    imported(q,b'one\n    two','TABS.TXT',7)
    open_import(q,7);choose(q,'LONG.TXT',7);q.choose_name('long.txt');q.widget(82,7)
    long_note=('строка\n'*2048).encode()
    imported(q,long_note,'long.txt',7);q.widget(42,7)
    assert local_read('long.txt')==long_note and q.document(7)['lines']==2049
    open_import(q,7);choose(q,'TENK.TXT',7);q.choose_name('rows.txt');q.widget(82,7)
    tenk=b'\n'*10000+'конец'.encode();imported(q,tenk,'rows.txt',7)
    q.key('ctrl-a');q.key('right');q.key('ret')
    assert q.document(7)['text']==tenk+b'\n' and q.document(7)['lines']==10002
    q.widget(77,7);assert q.document(7)['text']==tenk
    q.widget(42,7);assert local_read('rows.txt')==tenk
    q.screenshot(smoke.WORK/'10001-line-gutter.png')
    q.open_app(9);assert q.document(9)['text']==proof.replace(b'42',b'77')
    open_import(q)
    before=snap(q)
    for _ in range(12):
        refresh(q)
        assert q.mode(9)==6 and state(q).count and q.num('disk_count')==1
        assert snap(q)==before
    choose(q,'HELLO.C');q.choose_name('unplug.c');before=snap(q);files=q.files()
    q.cmd('device_del stick');time.sleep(.25)
    started=time.monotonic();q.widget(82,9)
    assert time.monotonic()-started<4 and q.mode(9)==6 and snap(q)==before and q.files()==files
    refresh(q,False);assert state(q).count==0 and q.num('disk_count')==0
    assert b'Error:' not in q.cmd(f'drive_add 0 if=none,id=flash,file={source},format=raw')
    assert b'Error:' not in q.cmd(f'device_add usb-storage,bus=usbtest.0,drive=flash,id=stick,port={PORT}');time.sleep(.25)
    refresh(q);assert state(q).count and q.num('disk_count')==1
    choose(q,'HELLO.C');q.choose_name('hot.c');q.widget(82,9);imported(q,proof,'hot.c')
    print('  Independent Notepad, UTF-8, tabs, 12 rescans, unplug/replug+Refresh PASS',flush=True)
# Reboot without USB: imported source actually persisted to the local disk.
with smoke.Guest() as q:
    assert 'proof.c' not in q.files()
    q.text('guimenu\n');time.sleep(1);q.open_app(9)
    q.widget(41,9);q.widget(84,9);q.choose_name('proof.c');q.widget(82,9)
    assert q.document(9)['text']==proof
    q.widget(91,9);time.sleep(.3)
    assert q.num('loaded')==1 and q.num('app_faulted')==0
    assert q.mem('app_status',96).split(b'\0')[0]==b'USB import OK'
    q.close_app(10);q.open_app(9)
    q.widget(41,9);q.choose_name('many.c');q.widget(82,9)
    assert q.document(9)['text']==b'// line\n'*512+proof+b'\n'
    q.widget(91,9);time.sleep(.3)
    assert q.num('loaded')==1 and q.num('app_faulted')==0
    assert q.mem('app_status',96).split(b'\0')[0]==b'USB import OK'
    q.close_app(10);q.open_app(7)
    q.widget(41,7);q.choose_name('long.txt');q.widget(82,7)
    assert q.document(7)['text']==('строка\n'*2048).encode()
    print('  Full reboot: reopen >512-line C + 2049-line UTF-8 note from KvFS; native Run PASS',flush=True)
# Raw/superfloppy FAT32 and no ATA disk; import + execution still work.
with smoke.Guest(False,extra_args=usb_args(raw)) as q:
    assert '/mnt/usb0 ' in q.log.read_text(errors='replace')
    q.text('vidmode 640 480\n');q.text('lang ru\n');q.text('guimenu\n');time.sleep(1);q.open_app(9)
    open_import(q);choose(q,'HELLO.C');q.widget(82,9);imported(q,proof,'HELLO.c')
    before=snap(q);q.widget(109,9)
    assert snap(q)==before and state(q).imported and not state(q).disk_valid
    assert b'KvFS' in state(q).status
    print('  Import Keep: KvFS without disk: explicit warning, no fake save PASS',flush=True)
    q.widget(91,9);time.sleep(.3)
    assert q.num('loaded')==1 and q.num('app_faulted')==0
    assert q.mem('app_status',96).split(b'\0')[0]==b'USB import OK'
    q.screenshot(smoke.WORK/'diskless-640x480.png')
    print('  Raw FAT32 USB, diskless RAM Build/Run, Russian 640x480 PASS',flush=True)
assert all(digest(p)==h for p,h in original.items()),'USB SOURCE IMAGE WAS MODIFIED'
print('  All source USB images SHA-256 unchanged PASS',flush=True)
