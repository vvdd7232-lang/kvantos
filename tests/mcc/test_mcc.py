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
        # API pointer slots: rgb is slot 3, clear 4, text 9, log 31.
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
# Bounded fixed arrays: values, native pointer width, decay, padding and canaries.
for scope in ('local','global'):
    for t,n,expression,want in [('int',7,'a[0]+a[6]',42),
                                ('char',7,'a[0]+a[6]',42),
                                ('int',7,'sizeof a',28),
                                ('char',7,'sizeof(a)',7),
                                ('int',7,'sizeof(a+0)',WORD),
                                ('int',7,'sizeof a[0]',4),
                                ('char',7,'sizeof a[0]',1),
                                ('int',7,'(a+6)-a',6)]:
        decl=f'{t} a[{n}];'
        body='a[0]=17;a[6]=25;return '+expression+';'
        pre=(decl+'int f(void){'+body+'}' if scope=='global' else
             'int f(void){'+decl+body+'}')
        check('f()',want,pre)
check('f()',42,'int f(void){int lo=9;char a[3];int hi=11;a[0]=20;a[2]=2;return lo+hi+a[0]+a[2];}')
check('f()',42,'int a[3],b=42;int f(void){return a[0]+a[1]+a[2]+b;}')
check('f()',42,'char a[3],b=42;int f(void){return a[0]+a[1]+a[2]+b;}')
check('f()',7,'int f(void){int x=7;int *a[3];a[0]=&x;return *a[0];}')
check('f()',3*WORD,'int f(void){int *a[3];return sizeof a;}')
check('f()',3*WORD,'char *a[3];int f(void){return sizeof(a);}')
check('f()',21,'int f(void){int a[2];a[0]=9;a[1]=12;return g(a);}int g(int *p){return p[0]+p[1];}')
check('f()',WORD,'int f(void){int a[2];return g(a);}int g(int *p){return sizeof p;}')
check('f()',65,'int f(void){char a[2];char *p=a;a[0]=65;p[1]=0;return *p;}')
check('f()',7,'int f(void){int a[2];int *p=&a[0];p[1]=7;return a[1];}')
check('f()',-1,'int f(void){char a[2];a[0]=255;return a[0];}')
check('f()',42,'int *a[2];int x=42;int f(void){a[1]=&x;return *a[1];}')
check('f()',42,'int f(void){char a[0x10];int b[010];a[15]=17;b[7]=25;return a[15]+b[7];}')
check('f()',42,'int f(void){char a[1],b[1];a[0]=19;b[0]=23;return a[0]+b[0];}')
for expression,want in [('sizeof(int)',4),('sizeof(char)',1),('sizeof(int*)',WORD),
                        ('sizeof(void*)',WORD),('sizeof(char**)',WORD),
                        ('sizeof(1+2)',4),('sizeof \'a\'',4),('sizeof "abc"',4),
                        ('sizeof("a\\0b")',4),('sizeof("abc"[0])',1),
                        ('sizeof(sizeof(int))',4)]:
    check(expression,want)
# sizeof discards calls, assignments, increments, data and relocation emission.
check('f()',17,'int n=9;int change(void){n=100;return 1;}int f(void){int z=sizeof(change());int k=sizeof(n++);return n+z+k;}')
check('f()',9,'int f(void){int n=9;int z=sizeof(n=42);return n;}')
check('f()',9,'int f(void){int n=9;int z=sizeof(++n);return n;}')
check('f()',WORD+9,'int f(void){int n=9;int *p=&n;int z=sizeof(p++);return z+*p;}')
check('f()',WORD,'int f(void){return sizeof(alloc(64));}')
check('f()',4,'int f(void){return sizeof(1/0);}')
check('f()',5,'int f(void){char a[5];return sizeof((a));}')
check('f()',4,'int f(void){int *p=0;return sizeof(*p);}')
check('f()',112,'int f(void){int n=sizeof("hidden");return "live"[1]+n;}')
check('f()',6,'int f(void){int x=7;return sizeof x+2;}')
app=App('void draw(void){clear(sizeof(alloc(64)));}')
assert app.result()==WORD and [slot for slot,args in app.calls]==[4],app.calls
passed+=1
check('f()',7000,'char a[7000];int f(void){return sizeof a;}')
check('f()',128*WORD,'int f(void){char a[LOCAL_BYTES];return sizeof a;}'.replace('LOCAL_BYTES',str(128*WORD)))
check('f()',42,'int f(void){int a[2];return g(a);}int g(int *p){p[0]=42;return p[0];}')
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

# Bounded object-like preprocessing: textual precedence, token boundaries, scope.
check('ANSWER',42,'#define ANSWER 42')
check('2*TERM',5,'#define TERM 1+3')
check('2*TERM',8,'#define TERM (1+3)')
check('A',42,'#define A B\n#define B 42')
check('ANSWER',42,'  /* before */ #define ANSWER 42 // trailing comment')
check('ANSWER',42,'#define ANSWER 40 /* gap */ +2')
check('f()',42,'#define COUNT 3\nint f(void){int a[COUNT];a[2]=42;return a[2];}')
check('f()',42,'#define T int\nT f(void){T x=42;return x;}')
check('f()',4,'#define SIZE sizeof\nint f(void){return SIZE(int);}')
check('f()',3,'#define NAME a\nint f(void){int a=3;return NAME;}')
check('f()',42,'#define EMPTY\nint f(void){EMPTY return 42;}')
check('f()',65,'#define LETTER \'A\'\nint f(void){return LETTER;}')
check('f()',65,'#define A 7\nint f(void){return "A"[0];}')
check('f()',65,'#define A 7\nint f(void){return \'A\';}')
check('f()',42,'#define A 17\nint f(void){int AA=42;return AA;}')
check('f()',3,'#define PLUS +\nint f(void){int x=2;return x PLUS+1;}')
check('f()',3,'#define ID x\nint f(void){int x=2;return ID+1;}')
check('f()',42,'#define TEXT "/* A // */"\nint f(void){return sizeof(TEXT)+31;}')
check('f()',42,'#define NUM 40\nint f(void){return NUM+2;}\n')
check('f()',42,'#define X 42\r\nint f(void){return X;}')
check('f()',42,'int f(void){\n#define HERE 42\nreturn HERE;}')
check('f()',42,'/* multiline\n#define NOT_A_DIRECTIVE 5\n*/\nint f(void){return 42;}')
check('f()',42,'// #define X 17\nint f(void){return 42;}')
check('A0',42,'#define A7 42\n'+''.join(f'#define A{i} A{i+1}\n' for i in range(6,-1,-1)))
check('M31',31,''.join(f'#define M{i} {i}\n' for i in range(32)))
# Bounded structs/typedef: native layout, member chains, aggregates by pointer only.
check('f()',42,'struct S{int x;char y;};int f(void){struct S s;s.x=40;s.y=2;return s.x+s.y;}')
check('f()',42,'struct S{int x;};struct S s;int f(void){s.x=42;return s.x;}')
check('f()',0,'struct S{int x;char y;};struct S s;int f(void){return s.x+s.y;}')
check('f()',42,'struct S{int x;};int f(void){struct S s;struct S *p=&s;p->x=42;return s.x;}')
check('f()',42,'struct S{int x;};int f(void){struct S s;s.x=42;return (&s)->x;}')
check('f()',42,'struct I{int n;};struct O{char c;struct I i;};int f(void){struct O o;o.i.n=42;return o.i.n;}')
check('f()',42,'struct I{int n;};struct O{struct I *p;};int f(void){struct I i;struct O o;o.p=&i;o.p->n=42;return i.n;}')
check('f()',42,'struct S{int a[3];char b[5];};int f(void){struct S s;s.a[2]=40;s.b[4]=2;return s.a[2]+s.b[4];}')
check('f()',12,'struct S{int a[3];};int f(void){struct S s;return sizeof(s.a);}')
check('f()',20,'struct S{int a[3];char b[5];};int f(void){return sizeof(struct S);}')
check('f()',12,'struct S{char a[5];int x;};int f(void){struct S s;return sizeof s;}')
check('f()',48,'struct S{char a[5];int x;};int f(void){struct S s[4];return sizeof s;}')
check('f()',42,'struct S{char a[5];int x;};int f(void){struct S a[4];a[3].x=42;return a[3].x;}')
check('f()',3,'struct S{char a[5];int x;};int f(void){struct S a[4];return &a[3]-a;}')
check('f()',-3,'struct S{char a[5];int x;};int f(void){struct S a[4];return a-&a[3];}')
check('f()',42,'struct S{char a[132];int x;};int f(void){struct S a[2];struct S *p=a;++p;p->x=42;p--;return a[1].x;}')
check('f()',42,'struct S{char a[132];int x;};int f(void){struct S a[2];struct S *p=a;p++;p->x=42;return (p--)->x;}')
check('f()',42,'struct S{int x;};int g(struct S *p){return p->x;}int f(void){struct S s;s.x=42;return g(&s);}')
check('f()',42,'struct S{int x;};struct S s;struct S *get(void){return &s;}int f(void){get()->x=42;return s.x;}')
check('f()',42,'struct Node;typedef struct Node Node;struct Node{int n;Node *next;};int f(void){Node a,b;a.next=&b;b.n=42;return a.next->n;}')
check('f()',42,'typedef int Number;typedef Number *Ptr;int f(void){Number n=42;Ptr p=&n;return *p;}')
check('f()',42,'typedef struct {int n;} Item;int f(void){Item i;i.n=42;return i.n;}')
check('f()',42,'typedef struct S{int n;} Item,*IP;int f(void){Item i;IP p=&i;p->n=42;return i.n;}')
check('f()',42,'typedef int N,*P;int f(void){N n=42;P p=&n;return *p;}')
check('f()',42,'typedef char Byte;int f(void){Byte a[3];a[2]=42;return a[2];}')
check('f()',WORD,'struct S;typedef struct S *SP;int f(void){return sizeof(SP);}')
check('f()',WORD,'struct S{int n;};int f(void){return sizeof(struct S*);}')
check('f()',42,'#define N 2\ntypedef struct {int n[N];} Item;int f(void){Item a;a.n[1]=42;return a.n[1];}')
check('f()',42,'struct T{int x;};typedef int T;int f(void){T n=42;return n;}')
check('f()',5,'struct S{int x;};int f(void){struct S s;s.x=5;int n=sizeof(s.x++);return s.x;}')
check('f()',42,'struct S{int x;};struct S *p=0;int f(void){return sizeof(p->x)+38;}')
# Independent ctypes layout oracle uses target-width integers in pointer slots.
class Layout(C.Structure):
    _fields_=[('head',C.c_char),('n',C.c_int32),('p',C.c_uint64 if IS64 else C.c_uint32),('tail',C.c_char)]
size=C.sizeof(Layout)
check('sizeof(struct S)',size,'struct S{char head;int n;int *p;char tail;};')
app=App('struct S{char head;int n;int *p;char tail;};struct S s;void draw(void){s.head=17;s.n=42;s.p=&s.n;s.tail=23;log(&s);clear(sizeof s);}')
assert app.result()==size
address=[args[0] for slot,args in app.calls if slot==31][-1]
raw=bytes(app.uc.mem_read(address,size))
assert raw[Layout.head.offset]==17 and raw[Layout.tail.offset]==23
assert struct.unpack_from('<i',raw,Layout.n.offset)[0]==42
assert struct.unpack_from('<Q' if IS64 else '<I',raw,Layout.p.offset)[0]==address+Layout.n.offset
passed+=1
class Outer(C.Structure):
    _fields_=[('head',C.c_char),('inner',Layout),('tail',C.c_char)]
check('sizeof(struct O)',C.sizeof(Outer),'struct S{char head;int n;int *p;char tail;};struct O{char head;struct S inner;char tail;};')
bad=['', 'void draw( {', 'void draw(void){missing();}', 'void draw(void){x=1;}',
     'void draw(void){int x=;}', 'void draw(void){1=2;}', 'void draw(void){break;}',
     'void draw(void){clear();}', 'void draw(void){clear(1,2);}', '#include "kvapp.h"',
     'void draw(void){char *p="oops;}', '/* missing end', 'int f(void);void draw(void){clear(f());}',
     'void draw(int x){}', 'int x;int x;', 'void draw(void){}void draw(void){}',
     'void draw(void){int x;int x;}', 'int f(void);int f(int x){return x;}void draw(void){}',
     'void draw(void){int a[0];}', 'void draw(void){float x;}', 'char f(void){return;}void draw(void){}',
     'void draw(void){return 3;}',
     'void draw(void){clear(4294967296);}', 'void draw(void){clear(09);}',
     'void draw(void){clear('+('('*50)+'1'+(')'*50)+');}',
     'void draw(void){'+('clear(1);'*1200)+'}']
bad += ['void draw(void){int a[-1];}', 'void draw(void){int a[];}',
        'void draw(void){int n=2;int a[n];}', 'void draw(void){int a[1+1];}',
        'void draw(void){int a[2][3];}', 'void draw(void){void a[3];}',
        'void draw(void){int a[2]={1,2};}', 'char a[4]="abc";void draw(void){}',
        'int a[4294967295];void draw(void){}', 'int a[2001];void draw(void){}',
        'char a[4000],b[4000];void draw(void){}',
        'void draw(void){char a['+str(128*WORD+1)+'];}',
        'void draw(void){char a['+str(128*WORD)+'];int b;}',
        'void draw(void){int a[2];a=0;}', 'void draw(void){int a[2];a++;}',
        'void draw(void){int a[2];&a;}', 'void f(int a[2]){}void draw(void){}',
        'void draw(void){sizeof(void);}', 'void draw(void){sizeof unknown;}',
        'void draw(void){sizeof(missing());}', 'void draw(void){sizeof(clear(1));}',
        'void draw(void){sizeof(int[3]);}', 'void draw(void){sizeof();}']
bad += ['#define F(x) x\nvoid draw(void){}', '#define X 1\n#define X 1\nvoid draw(void){}',
        '#define X X\nvoid draw(void){clear(X);}',
        '#define A B\n#define B A\nvoid draw(void){clear(A);}',
        '#undef X\nvoid draw(void){}', '#ifdef X\nvoid draw(void){}\n#endif',
        '#define X ##\nvoid draw(void){}', '#define X #y\nvoid draw(void){}',
        '#define X \\\n42\nvoid draw(void){}', '#define 9X 42\nvoid draw(void){}',
        'void draw(void){ #define X 42\n}',
        '#define X "unterminated\nvoid draw(void){}',
        '#define X /* multiline\n*/42\nvoid draw(void){}',
        '#define X '+('x'*256)+'\nvoid draw(void){}',
        '#define '+('x'*48)+' 42\nvoid draw(void){}',
        ''.join(f'#define M{i} {i}\n' for i in range(33))+'void draw(void){}',
        ''.join(f'#define A{i} A{i+1}\n' for i in range(8))+'#define A8 42\nvoid draw(void){clear(A0);}',
        '#define X '+('1 '*250)+'\nvoid draw(void){}',
        '#define X '+(' '*2)+'1\nvoid draw(void){clear(0xX);}',
        '#define X 1\nvoid draw(void){clear(12X);}',
        '#define B '+(' '*0)+'1+1+1+1+1+1+1+1\n'+
        '#define A '+('B '*100)+'\nvoid draw(void){'+('A '*50)+'}',
        '#define define 42\n#nope x\nvoid draw(void){}']
bad += ['struct S{};void draw(void){}', 'struct S{void x;};void draw(void){}',
        'struct S{struct S s;};void draw(void){}', 'struct S;struct S s;void draw(void){}',
        'struct S{int x;int x;};void draw(void){}', 'struct S{int x:3;};void draw(void){}',
        'struct S{int x;};struct S{int y;};void draw(void){}',
        'struct S{char a[8000];char b;};void draw(void){}',
        'struct S{int x;};void draw(void){struct S a,b;a=b;}',
        'struct S{int x;};void draw(void){struct S a=1;}',
        'struct S{int x;};struct S a=1;void draw(void){}',
        'struct S{int x;};void draw(void){struct S a;clear(a);}',
        'struct S{int x;};struct S f(void);void draw(void){}',
        'struct S{int x;};void f(struct S s){}void draw(void){}',
        'struct S{int x;};void draw(void){struct S a;a.missing=1;}',
        'struct S{int x;};void draw(void){struct S a;a->x=1;}',
        'struct S{int x;};void draw(void){int a;a.x=1;}',
        'struct S;void draw(void){struct S *p;p->x=1;}',
        'void draw(void){sizeof(struct S);}',
        'void draw(void){struct S{int x;}s;}',
        'struct S{struct I{int x;}i;};void draw(void){}',
        'typedef int T;typedef int T;void draw(void){}',
        'typedef int T;int T;void draw(void){}',
        'typedef int T;void draw(void){int T;}',
        'typedef int clear;void draw(void){}',
        'typedef int A[3];void draw(void){}',
        'typedef int (*F)(void);void draw(void){}',
        'void draw(void){typedef int T;}',
        ''.join(f'struct S{i};' for i in range(33))+'void draw(void){}',
        ''.join(f'typedef int T{i};' for i in range(33))+'void draw(void){}',
        'struct S{'+''.join(f'char x{i};' for i in range(257))+'};void draw(void){}',
        'void draw(void){sizeof(*3);}']
for source in bad:compile(source,False);passed+=1
for source,line_number in [('#define X 42\n\nvoid draw(void){missing();}',3),
                           ('#define X X\n\nvoid draw(void){clear(X);}',3),
                           ('/* a\nb */\n#define F(x) x\nvoid draw(void){}',3)]:
    compile(source,False);assert lib.mcc_error_line()==line_number,(source,lib.mcc_error_line())
    passed+=1
# Builtin headers are literal, readonly and include-once, not filesystem CPP.
for headers in ['#include "kvui.h"\n', '#include <kvui.h>\r\n',
                '#include "kvtypes.h"\n#include <kvui.h>\n#include "kvkeys.h"\n',
                '# include"kvui.h" /* one */ /**/ // two\n',
                '#include <kvui.h>\n'*300]:
    check('f()',42,headers+'int f(void){kv_point p;p.x=KV_CHAR_W;p.y=KV_CHAR_H;kv_int n=18;kv_byte c=0;return p.x+p.y+n+c;}')
check('f()',8,'#include <kvtypes.h>\nint f(void){return sizeof(kv_point);}')
check('f()',257,'#include <kvkeys.h>\nint f(void){return KV_KEY_DOWN;}')
check('f()',-1,'#include <kvui.h>\nint f(void){return KV_TRANSPARENT;}')
for header in ['kvapp.h','stdint.h','stdio.h','../kvui.h','/kvui.h','kvui\\.h',
               './kvui.h','KVUI.H','cycle1.h','depth1.h','', 'x'*32]:
    compile('#include "'+header+'"\nvoid draw(void){}',False);passed+=1
for directive in ['#include', '#', '#inc', '#include "kvui.h', '#include <kvui.h"',
                  '#include KV_HEADER', '#include "kvui.h" extra', '#include "kvui.h" /*',
                  '#include "kvui.h" #include "kvkeys.h"', '#includeevil "kvui.h"']:
    compile(directive+'\nvoid draw(void){}',False);passed+=1
for source,row in [('#include "kvui.h"\n\nvoid draw(void){missing();}',3),
                   ('#include "kvui.h"\n#define KV_BG 1\nvoid draw(void){}',2),
                   ('// header below\n#include "no.h"\nvoid draw(void){}',2),
                   ('#include "kvui.h"\n#define X X\nvoid draw(void){clear(X);}',3)]:
    compile(source,False);assert lib.mcc_error_line()==row,(source,lib.mcc_error_line());passed+=1
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
