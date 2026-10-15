#!/usr/bin/env python3
"""Real USB BOT/UHCI + FAT32 + mouse import + native build/run, both kernels.
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
from mkusb import make
smoke.WORK=ROOT/'build/usb-import'/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'local.img'
raw,mbr,proof=make(smoke.WORK)
subprocess.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=subprocess.DEVNULL)
sys.path.insert(0,str(ROOT/'sdk'))
from mkdisk import KvFS

class Doc(C.Structure):
    _fields_=[('buf',C.c_char*(128*121)),('ints',C.c_int*9)]
class Undo(C.Structure):
    _fields_=[('doc',Doc),('valid',C.c_int)]
class Entry(C.Structure):
    _fields_=[('name',C.c_char*128),('size',C.c_uint),('flags',C.c_ubyte*4),('year',C.c_ushort),('date',C.c_ubyte*4)]
class Editor(C.Structure):
    _pack_=8 if smoke.IS64 else 4
    _fields_=[('doc',Doc),('undo',Undo),('named',C.c_int),('mode',C.c_int),('pending',C.c_int),
              ('disk',C.c_int),('top',C.c_int),('caret',C.c_int),('rows',C.c_int),('cols',C.c_int),
              ('name',C.c_char*24),('status',C.c_char*192),('name_in',C.c_char*24),('pending_name',C.c_char*24),
              ('until',C.c_uint64),('path',C.c_char*256),('file',C.c_char*256),('items',Entry*128),
              ('count',C.c_int),('selected',C.c_int),('volume',C.c_int)]
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
def usb_args(image):return ['-device','piix3-usb-uhci,id=usbtest',
    '-drive',f'file={image},format=raw,if=none,id=flash',
    '-device','usb-storage,bus=usbtest.0,drive=flash,id=stick,port=1']
def local_read(name):
    entry=next(e for e in KvFS(str(smoke.DISK)).listing() if e['name']==name)
    with smoke.DISK.open('rb') as f:
        f.seek(entry['start']*512);return f.read(entry['size'])
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def imported(q,expected,name,app=9):
    assert q.mode(app)==0,(q.mode(app),state(q,app).status)
    assert q.document(app)['text']==expected,q.document(app)
    assert q.files()[name]==expected,(name,q.files().keys())

print('USB import:', 'UEFI x86-64' if smoke.IS64 else 'BIOS i386',flush=True)
original={p:digest(p) for p in (raw,mbr)}
with smoke.Guest(extra_args=usb_args(mbr)) as q:
    assert q.num('hc_count')==1 and q.num('disk_count')==1
    assert '/mnt/usb0p1' in q.log.read_text(errors='replace')
    q.text('lang ru\n');q.text('guimenu\n');time.sleep(1)
    q.open_app(9);before=snap(q)
    # Dirty guard and cancel leave the entire document + undo unchanged.
    q.widget(96,9);assert q.mode(9)==3;q.widget(83,9);assert snap(q)==before
    open_import(q);assert state(q).path==b'/mnt/usb0p1'
    choose(q,'Sources');assert state(q).path.endswith(b'/Sources')
    choose(q,'Nested');choose(q,'BOM.C');q.choose_name('proof.c')
    q.screenshot(smoke.WORK/'import-dialog-ru.png')
    q.widget(82,9);imported(q,proof,'proof.c')
    q.widget(90,9);assert q.files()['proof.kapp'][:4]==b'KAPP'
    q.widget(91,9);time.sleep(.4)
    assert q.num('loaded')==1 and q.num('app_faulted')==0
    assert q.mem('app_status',96).split(b'\0')[0]==b'USB import OK'
    q.screenshot(smoke.WORK/'imported-running.png')
    q.close_app(10);q.open_app(9)
    saved=local_read('proof.c');assert saved==proof
    print('  MBR FAT32, folders, BOM/CRLF, rename, native Build/Run + KvFS save PASS',flush=True)
    # Every malformed input must preserve document AND undo AND RAM files.
    for name in ('BINARY.C','LARGE.C','WIDE.C','LINES.C','UTF16.C'):
        before=snap(q);files=q.files();open_import(q);choose(q,name);q.widget(82,9)
        assert q.mode(9)==6 and snap(q)==before and q.files()==files,(name,state(q).status)
        q.widget(83,9)
    print('  Binary, oversized, long line, too many lines, UTF-16: atomic rejection PASS',flush=True)
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
    assert b'Error:' not in q.cmd(f'drive_add 0 if=none,id=flash,file={mbr},format=raw')
    assert b'Error:' not in q.cmd('device_add usb-storage,bus=usbtest.0,drive=flash,id=stick,port=1');time.sleep(.25)
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
    print('  Full reboot: reopen imported source from KvFS and run PASS',flush=True)
# Raw/superfloppy FAT32 and no ATA disk; import + execution still work.
with smoke.Guest(False,extra_args=usb_args(raw)) as q:
    assert '/mnt/usb0 ' in q.log.read_text(errors='replace')
    q.text('vidmode 640 480\n');q.text('lang ru\n');q.text('guimenu\n');time.sleep(1);q.open_app(9)
    open_import(q);choose(q,'HELLO.C');q.widget(82,9);imported(q,proof,'HELLO.c')
    q.widget(91,9);time.sleep(.3)
    assert q.num('loaded')==1 and q.num('app_faulted')==0
    assert q.mem('app_status',96).split(b'\0')[0]==b'USB import OK'
    q.screenshot(smoke.WORK/'diskless-640x480.png')
    print('  Raw FAT32 USB, diskless RAM Build/Run, Russian 640x480 PASS',flush=True)
assert all(digest(p)==h for p,h in original.items()),'USB SOURCE IMAGE WAS MODIFIED'
print('  Both USB images SHA-256 unchanged PASS',flush=True)
