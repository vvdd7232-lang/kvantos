#!/usr/bin/env python3
"""Compile actual production structs, without libc/multilib runtime or execution."""
from pathlib import Path
import json, os, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/usb.c').read_text()
start=s.index('typedef struct {\n    volatile u32 *op;')
s=s[start:s.index('static hc_t hcs',start)]
result=[]
with tempfile.TemporaryDirectory() as tmp:
    for bits in (32,64):
        for lab in (0,1):
            p=Path(tmp)/'layout.c';o=p.with_suffix('.o')
            p.write_text('#include "kernel.h"\n'+s+
                'char disk_size[sizeof(usb_disk_t)];\nchar hc_size[sizeof(hc_t)];\n'+
                'char speed_offset[__builtin_offsetof(usb_disk_t,speed)+1];\n'+
                'char hub_size[sizeof(struct {usb_disk_t d;u16 blocked;u8 ports,protocol;})];\n')
            subprocess.run([os.environ.get('CC','gcc'),f'-m{bits}',f'-DKV_STORAGE_LAB={lab}',
                            '-ffreestanding','-I'+str(root/'include'),'-c',str(p),'-o',str(o)],check=True)
            text=subprocess.check_output(['nm','-S',str(o)],text=True)
            d={m[3]:int(m[1],16) for line in text.splitlines() if len(m:=line.split())==4}
            d['speed_offset']-=1
            expected=(48 if bits==64 else 40)+8*lab
            assert d['disk_size']==expected,d
            assert d['hc_size']==(24 if bits==64 else 20),d
            assert d['hub_size']==expected+(8 if bits==64 else 4),d
            assert d['speed_offset']==(43 if bits==64 else 39)+8*lab,d
            result.append(dict(bits=bits,storage_lab=lab,**d))
print(json.dumps(result,indent=2))
print('PASS actual usb_disk_t: speed occupies former padding; i386/x64 NORMAL+LAB sizes/HC/hub layout')
