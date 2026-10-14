#!/usr/bin/env python3
"""Execute generated i386 code in Unicorn, not just inspect .kapp headers.
Requires: pip install unicorn. The native compiler library is built by make.
"""
import os
import ctypes as C
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[2]
IS64 = os.environ.get('MCC_ARCH64') == '1'
WORD = 8 if IS64 else 4
REG_A, REG_SP = (UC_X86_REG_RAX,UC_X86_REG_RSP) if IS64 else (UC_X86_REG_EAX,UC_X86_REG_ESP)
ARG_REGS = [UC_X86_REG_RDI,UC_X86_REG_RSI,UC_X86_REG_RDX,UC_X86_REG_RCX,UC_X86_REG_R8,UC_X86_REG_R9]
lib = C.CDLL(str(Path(__file__).parent / ('build/libmcc64.so' if IS64 else 'build/libmcc.so')))
lib.mcc_compile.argtypes = [C.c_char_p,C.c_char_p,C.c_void_p,C.c_uint32,C.POINTER(C.c_uint32)]
lib.mcc_error.restype = C.c_char_p
BASE, API, STACK, STOP = 0xE00000,0xC00000,0xB00000,0xA00000
if IS64: STACK += 0x100000000
passed = 0

def compile(source, good=True):
    out = (C.c_ubyte * 32768)(*[0xA5]*32768)
    n = C.c_uint32(123)
    rc = lib.mcc_compile(source.encode(),b'Test',out,len(out),C.byref(n))
    if not good:
        assert rc == -1 and n.value == 0 and bytes(out) == b'\xA5'*32768, source
        assert lib.mcc_error() and lib.mcc_error_line() >= 1
        return
    assert rc == 0, (source,lib.mcc_error(),lib.mcc_error_line())
    blob=bytes(out[:n.value])
    magic,version,hs,abi,flags,base,entry,code,bss,name=struct.unpack('<4sHH6I32s',blob[:64])
    assert (magic,version,hs,abi,flags,base,entry,bss)==(b'KAPP',1,64,3,257 if IS64 else 1,BASE,BASE,0)
    assert len(blob)==64+code
    return blob

class App:
    def __init__(self, source):
        blob=compile(source)
        self.uc=Uc(UC_ARCH_X86,UC_MODE_64 if IS64 else UC_MODE_32)
        for addr,size in [(BASE,0x200000),(API,0x10000),(STACK,0x10000),(STOP,0x1000)]:
            self.uc.mem_map(addr,size)
        self.uc.mem_write(BASE,blob[64:])
        self.calls=[]
        # API pointer slots: rgb is slot 3, clear 4, text 9, log 33.
        for slot in range(1,36):
            target=API+0x1000+slot*16
            self.uc.mem_write(API+slot*WORD,struct.pack('<Q' if IS64 else '<I',target))
            self.uc.mem_write(target,b'\xc3')
        self.uc.hook_add(UC_HOOK_CODE,self.hook)
        descriptor=self.run(BASE,[API])
        fields=struct.unpack('<Qii6Q' if IS64 else '<9I',self.uc.mem_read(descriptor,64 if IS64 else 36))
        self.callbacks=dict(zip(['open','draw','key','click','tick','close'],fields[3:]))
        self.run(self.callbacks['open'])

    def hook(self,uc,address,size,extra):
        if API+0x1000<=address<API+0x2000:
            slot=(address-API-0x1000)//16
            esp=uc.reg_read(REG_SP)
            if IS64:
                assert esp % 16 == 8, ('misaligned SysV call',slot,hex(esp))
                args=tuple(uc.reg_read(r) for r in ARG_REGS)+struct.unpack('<2Q',uc.mem_read(esp+8,16))
            else: args=struct.unpack('<8I',uc.mem_read(esp+4,32))
            self.calls.append((slot,args))
            result=0
            if slot==1:result=480
            if slot==2:result=300
            if slot==3:result=(args[0]<<16)|(args[1]<<8)|args[2]
            if slot==12:result=API+0x3000 # alloc, fixed scratch buffer for tests
            uc.reg_write(REG_A,result)

    def run(self,address,args=()):
        if not address:return 0
        sp=STACK+0xF000+(8 if IS64 else 0)
        if IS64:
            self.uc.mem_write(sp,struct.pack('<Q',STOP))
            for r,v in zip(ARG_REGS,args):self.uc.reg_write(r,v)
            saved=[UC_X86_REG_RBX,UC_X86_REG_RBP,UC_X86_REG_R12,UC_X86_REG_R13,UC_X86_REG_R14,UC_X86_REG_R15]
            for i,r in enumerate(saved):self.uc.reg_write(r,0xFEDC00000000+i)
        else:self.uc.mem_write(sp,struct.pack('<'+'I'*(len(args)+1),STOP,*[x&0xFFFFFFFF for x in args]))
        self.uc.reg_write(REG_SP,sp)
        self.uc.emu_start(address,STOP,count=200000)
        assert self.uc.reg_read(REG_SP)==sp+WORD, 'stack imbalance'
        if IS64:
            for i,r in enumerate(saved): assert self.uc.reg_read(r)==0xFEDC00000000+i, 'callee-saved register corrupted'
        return self.uc.reg_read(REG_A)

    def result(self):
        self.calls.clear();self.run(self.callbacks['draw'])
        # clear(result) is a convenient way to expose integer test results.
        values=[args[0] for slot,args in self.calls if slot==4]
        assert values,'no clear() call'
        return values[-1] & 0xFFFFFFFF

def check(expression,want,preamble=''):
    global passed
    app=App(preamble+'\nvoid draw(void){clear('+expression+');}')
    got=app.result()
    assert got==want&0xFFFFFFFF,(expression,got,want)
    passed+=1

for expr,want in [('2+3*4',14),('(2+3)*4',20),('20-3-2',15),('20/3',6),
                  ('-20/3',-6),('-20%3',-2),('1<<5',32),('-8>>1',-4),
                  ('3<4',1),('3>=4',0),('4==4',1),('4!=4',0),('!5',0),('~0',-1),
                  ('0x1234&255',52),('5|2',7),('5^3',6),('010',8)]:check(expr,want)
check('sum(4,5,6)',456,'int sum(int a,int b,int c){return a*100+b*10+c;}')
check('fact(6)',720,'int fact(int n){if(n<=1)return 1;return n*fact(n-1);}')
check('f(5)',12,'int f(int x){return g(x)+2;} int g(int x){return x*2;}')
check('f()',18,'int f(void){int i=0,s=0;while(i<8){i++;if(i==3)continue;if(i==7)break;s+=i;}return s;}')
check('f()',18,'int f(void){int s=0;for(int i=0;i<8;i++){if(i==3)continue;if(i==7)break;s+=i;}return s;}')
check('f()',6,'int f(void){int a=3;int *p=&a;(*p)++;return a+2;}')
check('f()',3,'int f(void){int x=3;return x++;}')
check('f()',4,'int f(void){int x=3;return ++x;}')
check('f()',0,'int x=0;int f(void){if(0 && ++x)x=99;if(1 || ++x)return x;return 9;}')
check('f()',99,'int f(void){char *p="abc";return p[2];}')
check('f()',2,'int f(void){int a=0;int *p=&a;return (p+2)-p;}')
check('f()',7,'char *g="hello";int f(void){char c=255;if(g[0]==104)return c+8;return 0;}')
check('f()',11,'int f(void){int a=8;a*=3;a/=2;a-=1;return a;}')
check('f()',14,'int f(void){int a=2;int *p=&a;p+=3;return (p-&a)*4+a;}')
check('f()',-1,'char f(void){return 255;}')
check('f()',4,'int f(void){int s=0;for(int i=0;i<3;i++){for(int j=0;j<3;j++){if(j==1)break;s++;}}return s+1;}')
check('f()',9,'int f(void){int a=9;void *p=&a;return g(p);}int g(void *p){int *q=p;return *q;}')
check('f(1,2,3,4,5,6,7,8)',36,'int f(int a,int b,int c,int d,int e,int f,int g,int h){return a+b+c+d+e+f+g+h;}')
check('f()',12,'int f(void){int s=0;for(int i=0;i<6;i++){if(i%2)continue;s+=i*2;}return s;}')
check('f()',120,'int f(void){char *p="abc";p[1]=120;return p[1];}')
check('f()',4,'int f(void){int n=0;int *p=&n;int **q=&p;**q=4;return n;}')
app=App('void draw(void){text(10,20,"hello",rgb(1,2,3),-1);}')
app.run(app.callbacks['draw'])
text=[a for slot,a in app.calls if slot==9][-1]
assert text[0:2]==(10,20) and text[3]==0x010203 and text[4]&0xFFFFFFFF==0xFFFFFFFF
assert bytes(app.uc.mem_read(text[2],6))==b'hello\0'
passed+=1
app=App('int n=1;void key(int k){n+=k;}void click(int x,int y,int b){n=x+y+b;}void draw(void){clear(n);}')
app.run(app.callbacks['key'],[4]);assert app.result()==5
app.run(app.callbacks['click'],[20,30,1]);assert app.result()==51
passed+=1

bad=['', 'void draw( {', 'void draw(void){missing();}', 'void draw(void){x=1;}',
     'void draw(void){int x=;}', 'void draw(void){1=2;}', 'void draw(void){break;}',
     'void draw(void){clear();}', 'void draw(void){clear(1,2);}', '#include "kvapp.h"',
     'void draw(void){char *p="oops;}', '/* missing end', 'int f(void);void draw(void){clear(f());}',
     'void draw(int x){}', 'int x;int x;', 'void draw(void){}void draw(void){}',
     'void draw(void){int x;int x;}', 'int f(void);int f(int x){return x;}void draw(void){}',
     'void draw(void){int a[10];}', 'void draw(void){float x;}', 'char f(void){return;}void draw(void){}',
     'void draw(void){return 3;}',
     'void draw(void){clear(4294967296);}', 'void draw(void){clear(09);}',
     'void draw(void){clear('+('('*50)+'1'+(')'*50)+');}',
     'void draw(void){'+('clear(1);'*1200)+'}']
for source in bad:compile(source,False);passed+=1
# Repeated failure must not leak compiler state into a later successful build.
check('7*8',56)
# Pointer-width and builtin stack-argument regressions.
check('f()',17,'int f(void){int *p=alloc(64);p[0]=12;p[1]=5;return p[0]+p[1];}')
check('f()',8,'int f(void){int x=8;int *p=&x;int **q=&p;int **r=q+1;--r;return **r;}')
check('f()',6,'int f(void){int *p=alloc(64);p+=3;p[-1]=6;return *(p-1);}')
check('f()',9,'int f(void){int x=9;int *p=&x;int **q=&p;return **(1+q-1);}')
app=App('void draw(void){format(alloc(128),128,"%d %d %d %d %d",1,2,3,4,5);}')
app.run(app.callbacks['draw'])
args=[a for slot,a in app.calls if slot==29][-1]
assert args[:2]==(API+0x3000,128) and args[3:8]==(1,2,3,4,5),args
passed+=1
print(f'{passed} compiler/code-generation checks passed ({"x86-64" if IS64 else "i386"})')
