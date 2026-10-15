#!/usr/bin/env python3
"""Build and run native C apps INSIDE KvantOS; verify disk save after reboot.
Needs make iso, QEMU and Python. Only private tests/mcc/build/workshop.img is written.
"""
from pathlib import Path
import os
import socket
import shutil
import sys
import struct
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
os.chdir(ROOT)
IS64=os.environ.get('MCC_ARCH64')=='1'
WORK=ROOT/'tests/mcc/build'/('uefi64' if IS64 else 'bios32')
WORK.mkdir(parents=True,exist_ok=True)
DISK=WORK/'workshop.img'
symbols={}
for line in subprocess.check_output(['nm','-n','build/kvant64.bin' if IS64 else 'build/kvant.bin'],text=True).splitlines():
    parts=line.split()
    if len(parts)==3:symbols[parts[2]]=int(parts[0],16)

sys.path.insert(0,str(ROOT/'tests/boot'))
from gui_driver import MouseGUI

class Guest(MouseGUI):
    symbols=symbols
    def __init__(self, disk=True): self.disk = disk
    def __enter__(self):
        self.log=WORK/'workshop.serial.log'
        self.log.unlink(missing_ok=True)
        monitor=WORK/'workshop.sock'
        monitor.unlink(missing_ok=True)
        firmware=[]
        if IS64:
            vars_path=WORK/'OVMF_VARS.fd'
            shutil.copyfile(os.environ.get('OVMF_VARS','/usr/share/OVMF/OVMF_VARS_4M.fd'),vars_path)
            firmware=['-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/usr/share/OVMF/OVMF_CODE_4M.fd'),
                      '-drive','if=pflash,format=raw,file='+str(vars_path)]
        disk_args=[]
        if self.disk:
            if IS64:
                # q35's default IDE bus is AHCI; the OS supports legacy ATA PIO.
                disk_args=['-device','piix3-ide,id=kvide',
                           '-drive',f'file={DISK},format=raw,if=none,id=kvdata',
                           '-device','ide-hd,bus=kvide.0,drive=kvdata']
            else:disk_args=['-drive',f'file={DISK},format=raw,if=ide,index=0']
        self.proc=subprocess.Popen([
            'qemu-system-x86_64','-machine','q35,accel=tcg' if IS64 else 'pc,accel=tcg','-cpu','max' if IS64 else 'pentium','-m','512',*firmware,'-cdrom','release/kvantos.iso',
            *disk_args,
            '-boot','d','-display','none',
            '-serial','file:'+str(self.log),'-monitor','unix:'+str(monitor)+',server,nowait'
        ],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        deadline=time.monotonic()+40
        while time.monotonic()<deadline:
            if self.log.exists() and 'kvant:/' in self.log.read_text(errors='replace'):break
            if self.proc.poll() is not None:raise RuntimeError(self.proc.stderr.read())
            time.sleep(0.2)
        else:raise RuntimeError('boot timeout')
        self.sock=socket.socket(socket.AF_UNIX)
        self.sock.connect(str(monitor));self.sock.settimeout(5);self.response()
        if self.disk:assert 'KvFS mounted' in self.log.read_text(errors='replace'), 'test disk not mounted'
        else:assert 'No ATA disk' in self.log.read_text(errors='replace')
        return self
    def response(self):
        out=b''
        while b'(qemu) ' not in out:
            chunk=self.sock.recv(65536)
            if not chunk:raise RuntimeError('QEMU monitor closed')
            out+=chunk
        return out
    def cmd(self,text):
        self.sock.sendall((text+'\n').encode());return self.response()
    def key(self,name):
        self.cmd('sendkey '+name+' 40');time.sleep(0.1)
    def text(self,text):
        names={' ':'spc','\n':'ret','.':'dot','*':'shift-8','=':'equal',
               '!':'shift-1','_':'shift-minus','-':'minus','/':'slash',
               ':':'shift-semicolon',';':'semicolon',',':'comma',
               '(':'shift-9',')':'shift-0','{':'shift-bracket_left','}':'shift-bracket_right',
               '[':'bracket_left',']':'bracket_right','+':'shift-equal','<':'shift-comma','>':'shift-dot',
               '\"':'shift-apostrophe',"'":'apostrophe','#':'shift-3','%':'shift-5','&':'shift-7'}
        for c in text:self.key('shift-'+c.lower() if 'A'<=c<='Z' else names.get(c,c))
    def mem(self,name,size):
        at=symbols[name] if isinstance(name,str) else name
        path=WORK/'memory.bin';path.unlink(missing_ok=True)
        self.cmd(f'pmemsave 0x{at:x} {size} "{path}"')
        return path.read_bytes()
    def num(self,name):return struct.unpack('<I',self.mem(name,4))[0]
    def files(self):
        result={}
        stride=40 if IS64 else 36
        table=self.mem('files',32*stride)
        for off in range(0,len(table),stride):
            name,ptr,size,used=struct.unpack_from('<24sQII' if IS64 else '<24sIII',table,off)
            if used:result[name.split(b'\0')[0].decode()]=self.mem(ptr,size)
        return result
    def __exit__(self,*exc):
        self.sock.close();self.proc.terminate()
        try:self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait()

def main():
    print('Workshop integration:', 'UEFI x86-64' if IS64 else 'BIOS i386',flush=True)
    subprocess.run(['python3','sdk/mkdisk.py',str(DISK),'16'],check=True)
    sys.path.insert(0,str(ROOT/'sdk'))
    from mkdisk import KvFS
    proof_source = b'''int *values=0;
char *buffer=0;
int sum8(int a,int b,int c,int d,int e,int f,int g,int h){return a+b+c+d+e+f+g+h;}
int calculate(void){int x=5;int *p=&x;int **q=&p;return **q+sum8(1,2,3,4,5,6,7,8);}
void open(void){
    values=alloc(32);buffer=alloc(128);values[0]=calculate();
    format(buffer,128,"%d:%d:%d:%d:%d",values[0],2,3,4,5);
}
void draw(void){clear(rgb(24,32,50));text(10,20,buffer,-1,-1);status(buffer);}
void close(void){release(values);release(buffer);}
'''
    KvFS(str(DISK)).add('verify.c',proof_source,False)
    with Guest() as q:
        q.text('guimenu\n');time.sleep(1)
        q.open_app(7);q.text('private notes')
        q.open_app(9);time.sleep(0.3)
        q.widget(90,9);time.sleep(0.5)
        q.cmd(f'screendump "{WORK / "workshop.ppm"}"')
        files=q.files()
        assert 'hello.c' in files and 'hello.kapp' in files,files.keys()
        blob=files['hello.kapp']
        assert blob[:4]==b'KAPP'
        assert struct.unpack_from('<I',blob,12)[0] == (257 if IS64 else 1)
        (WORK/'guest-hello.kapp').write_bytes(blob)
        (WORK/'guest-hello.c').write_bytes(files['hello.c'])
        assert b'void draw(void)' in files['hello.c']
        # Editing contexts are independent even with two app types in one GUI.
        q.open_app(7)
        assert q.document(7)['text']==b'private notes'
        q.open_app(9);q.widget(91,9);time.sleep(1)
        assert q.num('loaded')==1
        assert q.mem('app_file',44).split(b'\0')[0]==b'hello.kapp'
        # Generated entry stores API in the first data word. The template's global
        # count is immediately after the descriptor and the "hello.c" title.
        data_base=struct.unpack_from('<I',blob,64+(1 if IS64 else 5))[0]
        count_address=data_base+(72 if IS64 else 40)+len('hello.c')+1
        assert q.num(count_address)==0
        # About, Workshop, generated application: third window starts at 258,116.
        w=q.window(10);q.move(w['x']+50,w['y']+100)
        q.cmd('mouse_button 1');time.sleep(0.3)
        q.cmd('mouse_button 0');time.sleep(0.2)
        q.cmd(f'screendump "{WORK / "running.ppm"}"')
        assert q.num(count_address)==1, ('count',q.num(count_address),'pointer',q.num('mx'),q.num('my'))
        # Return to Workshop, introduce an error and ensure F6 cannot run stale code.
        # Close the program with its cross and return through the mouse launcher.
        q.close_app(10);q.open_app(9)
        q.key('home');q.text('bad syntax ');q.widget(91,9);time.sleep(0.3)
        assert q.mem('error_text',8 if IS64 else 4)!=b'\0'*(8 if IS64 else 4)
        assert q.num('win_count')==3,'failed build unexpectedly opened an app window'
        q.cmd(f'screendump "{WORK / "error.ppm"}"')
        print('Guest: build + .kapp load + native C draw/click + independent editors + error path OK')
    
    saved={e['name'] for e in KvFS(str(DISK)).listing()}
    assert {'hello.c','hello.kapp','verify.c'}.issubset(saved),saved

    # Fresh machine: RAM files are gone. The mouse file dialog reopens from KvFS.
    with Guest() as q:
        assert 'hello.c' not in q.files()
        q.text('guimenu\n');time.sleep(1);q.open_app(9)
        q.widget(41,9);q.widget(84,9);q.choose_name('hello.c');q.widget(82,9);time.sleep(0.3)
        assert q.mode(9)==0
        assert q.document(9)['text'].startswith(b'// Click Build')
        assert q.mem('editor_text',32).startswith(b'// Click Build')
        q.widget(91,9);time.sleep(1)
        assert q.num('loaded')==1
        assert q.mem('app_file',44).split(b'\0')[0]==b'hello.kapp'
        q.cmd(f'screendump "{WORK / "rebooted.ppm"}"')
        print('Guest: source reopened from disk and rebuilt after a full reboot OK')
        q.close_app(10);q.open_app(9)
        q.widget(41,9);q.choose_name('verify.c');q.widget(82,9);q.widget(91,9);time.sleep(1)
        assert q.num('loaded')==1 and q.num('app_faulted')==0
        assert q.mem('app_file',40).split(b'\0')[0]==b'verify.kapp'
        assert q.mem('app_status',96).split(b'\0')[0]==b'41:2:3:4:5'
        q.cmd(f'screendump "{WORK / "abi-proof.ppm"}"')
        print('Guest: native locals + double pointers + heap + 8-argument call + variadic format OK')

    
    # No hard disk attached: the compiler still creates and runs a RAM .kapp.
    with Guest(disk=False) as q:
        q.text('lang ru\n');q.text('guimenu\n');time.sleep(1);q.open_app(9);q.widget(90,9);time.sleep(.3)
        q.cmd(f'screendump "{WORK / "workshop-ru.ppm"}"')
        q.widget(91,9);time.sleep(1)
        assert 'hello.kapp' in q.files() and q.num('loaded') == 1
        q.cmd(f'screendump "{WORK / "running-ru.ppm"}"')
        print('Guest: Russian UI + diskless RAM-only build and native .kapp execution OK')

if __name__ == '__main__':
    if 'MCC_ARCH64' in os.environ:main()
    else:
        for arch in ['0','1']:
            subprocess.run(['python3',str(Path(__file__).resolve())],env=dict(os.environ,MCC_ARCH64=arch),check=True)
