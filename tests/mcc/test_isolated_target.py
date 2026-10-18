#!/usr/bin/env python3
"""Check opt-in MCC image construction and transactional failure, both targets.
Native execution of this adapter is covered by isolated-workshop-smoke.py.
"""
from pathlib import Path
import ctypes as C,struct,subprocess
from isolated_source import SOURCE,REUSED_NATIVE_FAULTS
HERE=Path(__file__).resolve().parent
for arch in (0,1):
    lib=C.CDLL(str(HERE/'build'/('libmcc64.so' if arch else 'libmcc.so')))
    args=[C.c_char_p,C.c_char_p,C.c_void_p,C.c_uint32,C.POINTER(C.c_uint32)]
    lib.mcc_compile_target.argtypes=args+[C.c_int];lib.mcc_compile.argtypes=args;lib.mcc_error.restype=C.c_char_p
    checks=[0]
    def compile(source,mode=1,capacity=49152,good=True,title=b'Test'):
        out=(C.c_ubyte*(49152+32))(*([0xa5]*(49152+32)));size=C.c_uint32(999)
        result=lib.mcc_compile_target(source,title,C.byref(out,16),capacity,C.byref(size),mode)
        raw=bytes(out);assert raw[:16]==raw[-16:]==b'\xa5'*16
        if not good:
            assert result==-1 and size.value==0 and lib.mcc_error(),(source,result,size.value)
            assert raw==b'\xa5'*len(raw),'failed target compile changed output'
            checks[0]+=1;return
        assert result==0,(source,lib.mcc_error())
        assert 64<size.value<=capacity and raw[16+size.value:]==b'\xa5'*(len(raw)-16-size.value)
        checks[0]+=1;return raw[16:16+size.value]
    good=compile(SOURCE)
    h=struct.unpack('<4sHH6I32s8I',good[:96])
    magic,ver,header,api,flags,base,entry,payload,bss,title,text,data_va,data_file,data_bytes,bss2,stack,protocol,reserved=h
    assert (magic,ver,header,api,flags,base,protocol,reserved)==(b'KAPP',2,96,4,257 if arch else 1,0xe00000,1,0)
    assert 0x6000<=entry-base<text<=0x8000 and data_va==0x8000
    assert data_file>=text and data_file%16==0 and not any(good[96+text:96+data_file])
    assert len(good)==96+payload==96+data_file+data_bytes and bss==bss2 and data_va+data_bytes+bss<0x1e0000
    assert stack==16384 and title==b'Test'+b'\0'*28
    path=HERE/'build'/('isolated64.kapp' if arch else 'isolated32.kapp');path.write_bytes(good)
    subprocess.run([str(HERE/'build/test_user_image'),str(path),str(arch)],check=True,stdout=subprocess.DEVNULL)
    for source,_ in REUSED_NATIVE_FAULTS.values():compile(source)
    compile(SOURCE,capacity=len(good)-1,good=False)
    assert compile(SOURCE,capacity=len(good))==good
    for call in (b'beep(440,10)',b'log("x")'):
        source=b'void draw(void){'+call+b';}'
        compile(source,good=False)
        assert b'unavailable in isolated target' in lib.mcc_error()
        assert struct.unpack_from('<I',compile(source,mode=0),8)[0]==3,'isolated failure poisoned legacy target'
    compile(b'void draw(void){int h;int m;int s;clock(&h,&m,&s);clock(0,0,0);}')
    compile(b'void draw(void){line(-100,10,200,10,1);file_list(0,0,0);}')
    for headers in (b'#include <kvui.h>\n',b'#include "kvtypes.h"\n#include "kvui.h"\n'*100):
        compile(headers+b'void draw(void){kv_point p;p.x=KV_CHAR_W;clear(p.x);}')
    for header in (b'stdio.h',b'../kvui.h',b'kvapp.h',b'cycle1.h'):
        compile(b'#include "'+header+b'"\nvoid draw(void){}',good=False)
    compile(b'void draw(void){}',mode=2,good=False)
    compile(b'void draw(void){}',mode=-1,good=False)
    compile(b'bad syntax',good=False)
    compile(b'char big[8000];void draw(void){}',good=False)
    compile(b' '*16001,good=False)
    assert compile(b'\n'*150+b'void draw(void){clear(1);}')
    assert compile(b'void draw(void){}',title=b'x'*64)[32:64]==b'x'*31+b'\0'
    # Legacy public entry point must still force mode 0 after an isolated build.
    out=(C.c_ubyte*32768)();size=C.c_uint32()
    assert lib.mcc_compile(b'void draw(void){}',b'Old',out,len(out),C.byref(size))==0
    assert struct.unpack_from('<HHI',bytes(out),4)==(1,64,3)
    print('PASS isolated MCC', 'x64' if arch else 'i386',checks[0], 'target/layout/capacity/API rejection/reset checks; actual image validator PASS',flush=True)
