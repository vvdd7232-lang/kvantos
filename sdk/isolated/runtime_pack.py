#!/usr/bin/env python3
"""Embed fixed-address user-only adapters, not ELF/kernel pointers, for MCC."""
from pathlib import Path
import subprocess,sys
assert len(sys.argv)==4,'runtime_pack.py ELF32 ELF64 OUTPUT'
parts=['/* Generated user-only text. Rebuild from sdk/isolated/mcc_runtime.c. */\n']
for index,name in enumerate(sys.argv[1:3]):
    elf=Path(name);raw=elf.read_bytes();assert raw[:4]==b'\x7fELF' and raw[4]==index+1
    assert not subprocess.check_output(['nm','-u',str(elf)],text=True).strip()
    symbols={}
    for row in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
        p=row.split()
        if len(p)==3:symbols[p[2]]=int(p[0],16)
    out=elf.with_suffix('.text.bin')
    subprocess.run(['objcopy','-O','binary','--only-section=.text',str(elf),str(out)],check=True)
    text=out.read_bytes();assert symbols['__text_start']==0xe06000 and len(text)==symbols['__text_end']-0xe06000
    assert 0<len(text)<=8192 and symbols['__data_start']==0xe0b000
    assert 0xe0b000<=symbols['__memory_end']<0xfe0000
    parts.append('#if MCC_TARGET64\n' if index else '#if !MCC_TARGET64\n')
    parts.append(f'#define MCC_USER_ENTRY {symbols["_start"]}u\n#define MCC_USER_END {symbols["__memory_end"]-0xe00000}u\n')
    parts.append('static const u8 mcc_user_text[]={\n')
    for i in range(0,len(text),24):parts.append(','.join(str(b) for b in text[i:i+24])+',\n')
    parts.append('};\n#endif\n')
output=Path(sys.argv[3]);output.parent.mkdir(parents=True,exist_ok=True)
content=''.join(parts)
if not output.exists() or output.read_text()!=content:output.write_text(content)
