#!/usr/bin/env python3
"""Pack our fixed-address SDK ELF into strict KAPP format2 / experimental ABI4."""
from pathlib import Path
import subprocess,struct,sys
assert len(sys.argv) in (4,5),'pack.py ELF OUTPUT ARCH64 [PROTOCOL=1]'
protocol=int(sys.argv[4]) if len(sys.argv)==5 else 1
assert protocol in (1,2)
elf,out,arch=Path(sys.argv[1]),Path(sys.argv[2]),int(sys.argv[3]);assert arch in (0,1)
e=elf.read_bytes();assert e[:4]==b'\x7fELF' and e[4]==(2 if arch else 1)
symbols={}
for line in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
    parts=line.split()
    if len(parts)==3:symbols[parts[2]]=int(parts[0],16)
def section(name):
    p=out.with_suffix('.'+name+'.bin')
    subprocess.run(['objcopy','-O','binary','--only-section=.'+name,str(elf),str(p)],check=True)
    return p.read_bytes()
text=section('text');data=section('data')
assert symbols['__text_start']==0xe00000 and len(text)==symbols['__text_end']-0xe00000
va=symbols['__data_start']-0xe00000;assert len(data)==symbols['__data_end']-symbols['__data_start']
bss=symbols['__memory_end']-symbols['__data_end'];fileoff=(len(text)+15)&~15
payload=text+b'\0'*(fileoff-len(text))+data
assert 0<len(payload)<=65536 and va+len(data)+bss<=0x1e0000
header=struct.pack('<4sHH6I32s8I',b'KAPP',2,96,4,1|(256 if arch else 0),0xe00000,symbols['_start'],len(payload),bss,b'Shared pixels' if protocol==2 else b'Isolated boxes',len(text),va,fileoff,len(data),bss,16384,protocol,0)
assert len(header)==96
out.write_bytes(header+payload)
print(out,len(header+payload),f'bytes; format2 ABI4, isolated protocol{protocol}')
