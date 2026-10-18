#!/usr/bin/env python3
"""EN/RU set-1 -> real GUI, literal UTF-8 find/replace, savepoints and reboot.
Private disk only; BIOS install uses --keep on this disposable fixture.
"""
from pathlib import Path
import ctypes as C
import os,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2]
os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
from text_model import editor_type
Editor=editor_type(smoke.IS64)
smoke.WORK=ROOT/'build/editor-input'/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'input.img'
subprocess.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=subprocess.DEVNULL)
def state(q,app=7):return Editor.from_buffer_copy(q.mem('dev_editor' if app==9 else 'note_editor',C.sizeof(Editor)))
def wait_gui(q):
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        if q.num('scr_h')>0 and q.num('running'):return
        time.sleep(.1)
    raise AssertionError('GUI initialization timeout')
def same(q,s,app=7):assert q.document(app)['text']==s.encode(),(q.document(app),s,q.mode(app),list(state(q,app).undo.state),q.mem('keymap',24).hex())
def storage(q,ram,disk,app=7):
    s=state(q,app)
    assert bool(s.ram_valid and s.ram_revision==s.undo.state[4])==ram,(s.ram_revision,list(s.undo.state))
    assert bool(s.disk_valid and s.disk_revision==s.undo.state[4])==disk,(s.disk_revision,list(s.undo.state))
print('Editor input/find/storage:', 'UEFI x86-64' if smoke.IS64 else 'BIOS i386',flush=True)
with smoke.Guest() as q:
    q.text('lang ru\nguimenu\n');time.sleep(.4);q.open_app(7);q.key('ctrl-a')
    q.key('alt-shift');q.text('ghbdtn');same(q,'привет')
    q.key('caps_lock');q.text('g');same(q,'приветП');q.key('caps_lock');q.key('backspace')
    for key,ch in [('shift-g','П'),('grave_accent','ё'),('shift-grave_accent','Ё'),('shift-3','№')]:
        q.key(key);same(q,'привет'+ch);q.key('backspace');same(q,'привет')
    q.text(' ghbdtn');original='привет привет';same(q,original)
    q.key('ctrl-backspace');same(q,'привет приве');assert not q.mode();q.text('n');same(q,original)
    q.key('ctrl-z');same(q,'привет приве');q.key('ctrl-y');same(q,original)
    q.widget(101,7);q.text('!');same(q,original+'!');q.key('backspace');q.widget(101,7)
    q.widget(76,7);q.choose_name('ru.txt');assert state(q).name_in==b'ru.txt';q.widget(82,7)
    same(q,original);storage(q,True,True);assert not q.document()['dirty']
    print('  Alt+Shift/chip, RU/EN, Caps/Shift, ё/Ё/№, physical ASCII filename, Undo/Redo + KvFS PASS',flush=True)
    q.widget(102,7);assert q.mode()==8
    q.choose_name('ghbdtn');assert state(q).query=='привет'.encode()
    q.key('backspace');q.key('home');q.key('delete');q.text('g');q.key('end');q.text('n')
    assert state(q).query=='привет'.encode()
    q.widget(82,7);d=q.document();assert (d['ax'],d['cx'])==(0,12),d
    q.widget(82,7);d=q.document();assert (d['ax'],d['cx'])==(13,25),d
    q.widget(104,7);d=q.document();assert (d['ax'],d['cx'])==(0,12),d
    q.widget(104,7);assert q.document()['ax']==13
    q.key('ret');assert q.document()['ax']==0
    q.key('shift-ret');assert q.document()['ax']==13
    q.key('ret');assert q.document()['ax']==0
    q.widget(108,7);q.choose_name('vbh');assert state(q).replacement=='мир'.encode()
    q.widget(105,7);same(q,'мир привет');q.widget(83,7)
    q.widget(77,7);same(q,original);storage(q,True,True);assert not q.document()['dirty']
    q.widget(100,7);same(q,'мир привет');q.widget(77,7);same(q,original)
    q.key('ctrl-h');assert q.mode()==8 and state(q).search_focus==1
    q.widget(106,7);same(q,'мир мир');q.screenshot(smoke.WORK/'ru-find-replace.png');q.widget(83,7)
    q.widget(77,7);same(q,original);assert not q.document()['dirty']
    q.widget(100,7);same(q,'мир мир');assert q.document()['dirty'];storage(q,False,False)
    q.widget(42,7);storage(q,True,True)
    q.key('ctrl-a');q.widget(78,7)
    q.open_app(9);q.key('ctrl-a');q.widget(80,9);same(q,'мир мир',9)
    q.key('ctrl-f');assert q.mode(9)==8
    q.choose_name('vbh');q.widget(108,9);q.choose_name('ghbdtn');q.widget(106,9);same(q,original,9);q.widget(83,9)
    q.widget(77,9);same(q,'мир мир',9);q.widget(100,9);same(q,original,9)
    q.open_app(7);same(q,'мир мир');q.screenshot(smoke.WORK/'ru-editor-status.png')
    print('  UTF-8 fields, forward/backward/wrap, replace one/all, one Undo/savepoint, independent editors + clipboard PASS',flush=True)
    q.exit_gui();q.text('echo keyboardascii\n')
    assert q.log.read_text(errors='replace').count('keyboardascii')>=2
    if not smoke.IS64:
        q.text('setup --yes --keep\n')
        deadline=time.monotonic()+15
        while time.monotonic()<deadline and q.num('setup_percent')!=100:time.sleep(.1)
        assert q.num('setup_percent')==100,q.log.read_text(errors='replace')[-1200:]
        q.cmd('boot_set c');q.cmd('eject -f ide1-cd0')
    prompts=q.log.read_bytes().count(b'kvant:/')
    q.cmd('system_reset')
    deadline=time.monotonic()+45
    while time.monotonic()<deadline:
        if q.log.read_bytes().count(b'kvant:/')>prompts:break
        time.sleep(.2)
    else:raise AssertionError('reboot timeout')
    q.text('guimenu\n');time.sleep(.4);q.open_app(7);q.widget(41,7);q.choose_name('ru.txt');q.widget(82,7)
    same(q,'мир мир');storage(q,False,True)
    print('  RU-layout shell ASCII, real reboot/reopen and '+('standalone GRUB disk boot' if not smoke.IS64 else 'UEFI disk persistence')+' PASS',flush=True)
# No disk means no false persistence claim, even after Save.
with smoke.Guest(disk=False) as q:
    q.text('vidmode 640 480\nlang ru\nguimenu\n');wait_gui(q);q.open_app(7);q.key('ctrl-a');q.text('ram')
    q.widget(76,7);q.choose_name('ram.txt');q.widget(82,7)
    storage(q,True,False);assert not q.document()['dirty']
    q.key('ctrl-z');storage(q,False,False);q.key('ctrl-y');storage(q,True,False)
    q.screenshot(smoke.WORK/'ram-only-status.png')
    q.key('ctrl-a');q.text('aaa');before=bytes(state(q).doc)+bytes(state(q).undo)
    q.widget(102,7);q.choose_name('a');q.widget(108,7);q.choose_name('b'*41);q.widget(106,7)
    assert bytes(state(q).doc)+bytes(state(q).undo)==before
    q.screenshot(smoke.WORK/'find-limit-640x480.png');q.widget(83,7)
    print('  No-disk RAM-only status, Undo/Redo, 640x480 find/replace and atomic width-limit failure PASS',flush=True)

# Directory-full Save on a named RAM file: partial RAM success is not disk success.
sys.path.insert(0,str(ROOT/'sdk'))
from mkdisk import KvFS
k=KvFS(str(smoke.DISK),16)
for i in range(64):k.add(f"dummy{i:02d}.txt",b'x',False)
with smoke.Guest() as q:
    q.text('touch full.txt\nguimenu\n');wait_gui(q);q.open_app(7)
    q.widget(41,7);q.widget(94,7);q.choose_name('full.txt');q.widget(82,7)
    q.text('new');before=bytes(state(q).undo);q.widget(42,7)
    same(q,'new');storage(q,True,False)
    assert bytes(state(q).undo)==before and state(q).disk_failed and q.document()['dirty']
    assert q.files()['full.txt']==b'new'
    q.screenshot(smoke.WORK/'disk-full-status.png')
    print('  Disk-full: RAM saved, disk error persistent, document/history/savepoint preserved PASS',flush=True)
