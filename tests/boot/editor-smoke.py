#!/usr/bin/env python3
"""Mouse-first editor acceptance test on both real kernels, isolated private disk.
Only document/name text is typed. Launch, selection, dialogs, copy/paste, save,
build/run/help, confirmations, scrolling and GUI exit use actual mouse input.
"""
from pathlib import Path
import os,sys,struct,subprocess,time
ROOT=Path(__file__).resolve().parents[2]
os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
smoke.WORK=ROOT/'build/editor-smoke'/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
smoke.DISK=smoke.WORK/'editor.img'
sys.path.insert(0,str(ROOT/'sdk'))
from mkdisk import KvFS
subprocess.run([sys.executable,'sdk/mkdisk.py',str(smoke.DISK),'16'],check=True,stdout=subprocess.DEVNULL)
alpha=b'alpha beta\ngamma delta'
long=b'\n'.join((f'row{i:02d} '+('x'*114)).encode() for i in range(80))
k=KvFS(str(smoke.DISK))
for name,data in [('alpha.txt',alpha),('long.txt',long),('empty.txt',b''),('binary.txt',b'a\0b'),
                  ('large.txt',b'x'*121),('target.txt',b'protected'),('ru.txt','Привет мир'.encode())]+[(f'file{i:02d}.txt',f'file {i}'.encode()) for i in range(30)]:
    k.add(name,data,False)

def area(q,app=7):
    w=q.window(app)
    return w['x']+47,w['y']+22+1+96+4

def drag(q,x,y,xx,yy):
    q.move(x,y);q.cmd('mouse_button 1');time.sleep(.12)
    q.move(xx,yy);time.sleep(.12)
    q.cmd('mouse_button 0');time.sleep(.15)

def open_file(q,name):
    q.widget(41,7)
    if q.mode(7)==3:q.widget(84,7)
    q.choose_name(name);q.widget(82,7)
    assert q.mode(7)==0,(name,q.mode(7))

print('Mouse editors:', 'UEFI x86-64' if smoke.IS64 else 'BIOS i386',flush=True)
with smoke.Guest() as q:
    q.text('lang ru\n');q.text('guimenu\n');time.sleep(1)
    initial=q.num('win_count')
    q.text('gbetqws')
    assert q.num('win_count')==initial and q.num('running')==1,'letter launch/quit shortcut survived'
    icons=list(struct.iter_unpack('<3i',q.mem('icons',120)))
    ix,iy,_=next(t for t in icons if t[2]==7)
    q.click(ix+36,iy+20) # single-click desktop launch
    q.window(7)
    q.text(alpha.decode())
    x,y=area(q);q.click(x+6*8,y+7);q.text('X')
    assert q.document()['text']==b'alpha Xbeta\ngamma delta',(q.document(),x,y,q.window(7),q.num('mx'),q.num('my'))
    drag(q,x+6*8,y+7,x+5*8,y+16+7)
    d=q.document();assert (d['ay'],d['ax'],d['cy'],d['cx'])==(0,6,1,5),d
    q.widget(78,7)
    assert q.mem('ed_clipboard',64).split(b'\0')[0]==b'Xbeta\ngamma'
    q.screenshot(smoke.WORK/'notepad-selection.png')
    original=q.document()['text']
    q.widget(79,7);assert q.document()['text']==b'alpha  delta'
    q.widget(77,7);assert q.document()['text']==original
    q.widget(100,7);assert q.document()['text']==b'alpha  delta'
    q.widget(77,7);assert q.document()['text']==original
    q.open_app(9);template=q.document(9)['text']
    dx,dy=area(q,9);q.click(dx+3*8,dy+7);q.widget(80,9)
    assert b'Xbeta\ngamma' in q.document(9)['text']
    q.widget(77,9);assert q.document(9)['text']==template
    q.screenshot(smoke.WORK/'workshop.png')
    q.widget(92,9);q.screenshot(smoke.WORK/'workshop-help.png');q.widget(92,9)
    q.open_app(7);assert q.document()['text']==original
    q.widget(80,7);assert q.document()['text']==original
    q.widget(76,7);assert q.mode()==1
    q.choose_name('mouse.txt');q.widget(82,7)
    assert q.mode()==0 and q.document()['dirty']==0 and q.files()['mouse.txt']==original
    # Click a particular character, insert text, and protect New / Close / Quit.
    x,y=area(q);q.click(x+11*8,y+7);q.text('!');changed=q.document()['text']
    q.widget(40,7);assert q.mode()==3;q.widget(83,7)
    assert q.document()['text']==changed and q.document()['dirty']==1
    q.close_app(7);assert q.mode()==3;q.widget(83,7);q.window(7)
    q.widget(40,7);q.widget(82,7) # Save and continue New, no naming prompt needed
    assert q.document()['text']==b'' and q.files()['mouse.txt']==changed
    q.text('unsaved');q.widget(40,7);q.widget(84,7)
    assert q.document()['text']==b''
    q.widget(41,7);assert q.mode()==2
    q.widget(1200,7);q.screenshot(smoke.WORK/'file-dialog.png');q.widget(82,7)
    assert q.document()['text']==alpha # actual selected disk file, no typed path
    # Filename caret can be positioned by clicking, then edited in the middle.
    q.widget(76,7);q.choose_name('caret.txt')
    w=q.window(7);ny=w['y']+23+8+(w['h']-22-18)-78
    q.click(w['x']+25+72+5*8,ny+12);q.text('x')
    q.widget(82,7);assert 'caretx.txt' in q.files()
    # Binary/oversize loads never discard the current document.
    for name in ('binary.txt','large.txt'):
        before=q.document()['text'];q.widget(41,7);q.choose_name(name);q.widget(82,7)
        assert q.mode()==2 and q.document()['text']==before
        q.widget(83,7)
    open_file(q,'ru.txt');x,y=area(q)
    drag(q,x+8,y+7,x+4*8,y+7);q.widget(78,7)
    assert q.mem('ed_clipboard',32).split(b'\0')[0]=='рив'.encode()
    q.click(x+4*8,y+7);q.key('backspace');assert q.document()['text']=='Приет мир'.encode()
    q.widget(77,7);assert q.document()['text']=='Привет мир'.encode()
    open_file(q,'empty.txt');assert q.document()['text']==b'' 
    open_file(q,'alpha.txt');q.widget(76,7);q.choose_name('target.txt');q.widget(82,7)
    assert q.mode()==4 and 'target.txt' not in q.files()
    q.widget(83,7);assert q.mode()==1;q.widget(83,7)
    q.widget(76,7);q.choose_name('target.txt');q.widget(82,7);q.widget(82,7)
    assert q.mode()==0 and q.files()['target.txt']==alpha
    # Text scrolling must NOT chase the offscreen caret during repaint.
    open_file(q,'long.txt');assert q.document()['top']==0
    assert q.mem('packet_len',1)==b'\x04','wheel protocol was not negotiated'
    x,y=area(q);q.move(x+80,y+80);q.cmd('mouse_move 0 0 -2');time.sleep(.6)
    d=q.document();assert d['top']>0 and d['cy']==0,{k:v for k,v in d.items() if k!='text'}
    top=d['top'];time.sleep(.7);assert q.document()['top']==top
    vertical=next(t for t in q.hits(7) if t[4]==87);vx,vy,vw,vh,_,_=vertical
    q.click(vx+8,vy+vh-8);assert q.document()['top']==top+1
    # Vertical thumb drag to bottom, using the actual current geometry.
    d=q.document();rows=(vh-8)//16;maximum=d['lines']-rows;track=vh-32
    knob=max(16,track*rows//d['lines']);at=16+(track-knob)*d['top']//maximum
    drag(q,vx+8,vy+at+3,vx+8,vy+vh-18)
    assert q.document()['top']==maximum,q.document()
    horizontal=next(t for t in q.hits(7) if t[4]==88);hx,hy,hw,hh,_,_=horizontal
    q.click(hx+hw-8,hy+8);assert q.document()['left']>0
    # With maximum horizontal length 120, move the thumb to the far right.
    cols=(q.window(7)['w']-22-56)//8;maxleft=120-cols+1;tr=hw-32
    kn=max(16,tr*cols//(maxleft+cols));at=16+(tr-kn)*q.document()['left']//maxleft
    drag(q,hx+at+3,hy+8,hx+hw-18,hy+8)
    assert q.document()['left']==maxleft
    q.screenshot(smoke.WORK/'scrollbars.png')
    # Dragging past the text viewport continues selection and auto-scrolls.
    d=q.document();at=16+(track-knob)*d['top']//maximum
    drag(q,vx+8,vy+at+3,vx+8,vy+17)
    at=16+(tr-kn)*q.document()['left']//maxleft
    drag(q,hx+at+3,hy+8,hx+17,hy+8)
    x,y=area(q);drag(q,x+8,y+7,x+40,vy+vh+32)
    d=q.document();assert d['top']>0 and d['cy']>d['ay'] and d['selecting']

    # File-list wheel and arrow buttons reach files past the initial page.
    q.widget(41,7);q.move(q.window(7)['x']+150,q.window(7)['y']+180)
    q.cmd('mouse_move 0 0 -10');time.sleep(.4)
    rows=[t for t in q.hits(7) if t[4]>=1200];assert min(t[4] for t in rows)>1200
    q.widget(85,7);q.widget(86,7)
    rows=[t for t in q.hits(7) if t[4]>=1200];q.widget(max(t[4] for t in rows),7);q.widget(82,7)
    assert q.document()['text'].startswith(b'file ')
    # NumPad fallback uses the same mouse path. Physical movement cannot release
    # its latched synthetic button. NumLock is optional, never a launcher key.
    q.menu(item=10);assert q.num('mousekeys')==1
    q.move(12,12);before=q.num('mx');q.key('kp_6');assert q.num('mx')>before
    q.key('kp_0');assert q.mem('buttons',1)[0]&1
    q.cmd('mouse_move 3 3');time.sleep(.08);assert q.mem('buttons',1)[0]&1
    q.key('kp_decimal');assert not q.mem('buttons',1)[0]&1
    q.move(40,q.num('scr_h')-14);q.cmd('sendkey kp_5 40');time.sleep(.25)
    assert q.num('launcher_open')==1,'NumPad click did not open the menu'
    assert not q.mem('buttons',1)[0]&1,'NumPad click remained pressed'
    q.click(40,q.num('scr_h')-14)
    q.key('num_lock');assert q.num('mousekeys')==0
    # Quit is mouse-only and asks about BOTH independent dirty editors.
    q.open_app(7);q.text('!');q.menu(item=11);assert q.mode(7)==3
    q.widget(83,7);assert q.num('running')==1
    q.open_app(6);q.widget(102,6);q.widget(603,6)
    assert q.mode(7)==3;q.widget(83,7) # Settings exit must use the same guard
    q.menu(item=11);q.widget(84,7);assert q.mode(9)==3
    q.widget(83,9);assert q.num('running')==1
    q.exit_gui()
    assert 'Возврат в текстовую консоль kvsh.' in q.log.read_text(errors='replace')
    assert 'KERNEL PANIC' not in q.log.read_text(errors='replace')
    q.text('vidmode 640 480\n');q.text('guimenu\n');time.sleep(.5)
    assert q.num('scr_w')==640 and q.num('scr_h')==480
    for app in (7,9):
        q.open_app(app);w=q.window(app)
        assert w['x']>=0 and w['y']>=0 and w['x']+w['w']<=640 and w['y']+w['h']<=452
        q.widget(76,app);q.widget(83,app)
    q.widget(92,9);q.screenshot(smoke.WORK/'small-screen-help.png');q.widget(92,9)
    q.exit_gui()
    print('Mouse launch/caret/selection, shared clipboard, undo/redo, file dialogs, save/new/close/quit guards, overwrite, wheel, both scrollbars, file paging, MouseKeys PASS',flush=True)
# Dedicated multi-step history + saved checkpoint + independent editor state.
with smoke.Guest() as q:
    q.text('lang ru\n');q.text('guimenu\n');time.sleep(1);q.open_app(7)
    q.text('abc');assert q.document()['text']==b'abc'
    q.widget(77,7);q.widget(77,7);assert q.document()['text']==b'a'
    q.widget(100,7);assert q.document()['text']==b'ab'
    q.widget(100,7);assert q.document()['text']==b'abc'
    q.widget(76,7);q.choose_name('history.txt');q.widget(82,7)
    assert not q.document()['dirty']
    q.widget(77,7);assert q.document()['text']==b'ab' and q.document()['dirty']
    q.widget(100,7);assert q.document()['text']==b'abc' and not q.document()['dirty']
    q.widget(77,7);q.text('X');assert q.document()['text']==b'abX'
    q.widget(100,7);assert q.document()['text']==b'abX' and q.document()['dirty']
    q.widget(77,7);q.widget(77,7);assert q.document()['text']==b'a'
    q.widget(100,7);q.widget(100,7);assert q.document()['text']==b'abX'
    q.open_app(9);template=q.document(9)['text'];q.text('xyz')
    q.key('ctrl-z');assert q.document(9)['text']==b'xy'+template
    q.key('ctrl-y');assert q.document(9)['text']==b'xyz'+template
    q.open_app(7);assert q.document()['text']==b'abX'
    q.widget(42,7);assert not q.document()['dirty']
    q.widget(77,7);assert q.document()['dirty']
    q.widget(100,7);assert not q.document()['dirty']
    q.screenshot(smoke.WORK/'history-stack.png')
    # Opening a file resets history without touching the other editor.
    q.widget(41,7);q.choose_name('alpha.txt');q.widget(82,7)
    assert q.document()['text']==alpha and not q.document()['dirty']
    q.widget(77,7);q.widget(100,7);assert q.document()['text']==alpha
    q.open_app(9);assert q.document(9)['text']==b'xyz'+template
    q.widget(77,9);q.widget(77,9);q.widget(77,9);assert q.document(9)['text']==template
    q.widget(100,9);q.widget(100,9);q.widget(100,9);assert q.document(9)['text']==b'xyz'+template
    print('Multi-step mouse Undo/Redo, Ctrl+Z/Y, saved checkpoint, branching, independent histories, load reset PASS',flush=True)
# Check persisted contents only AFTER QEMU has closed the disk.
k=KvFS(str(smoke.DISK))
def persisted(name):
    entry=next(e for e in k.listing() if e['name']==name)
    with open(smoke.DISK,'rb') as f:
        f.seek(entry['start']*512);return f.read(entry['size'])
assert persisted('mouse.txt')==changed
assert persisted('target.txt')==alpha
assert persisted('history.txt')==b'abX'
print('Notepad RAM + real KvFS persistence PASS',flush=True)
