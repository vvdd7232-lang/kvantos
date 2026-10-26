#!/usr/bin/env python3
"""No guest/ISO required: low-memory collision guards and real E820 instructions.
Uses NASM when available, otherwise GNU as with a minimal Intel-syntax adapter.
The same direct_mmap.inc is executed by the actual boot loader and Unicorn.
"""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from direct_layout import read_layout, reserved_ranges, validate_bounce


class Layout(unittest.TestCase):
    def test_full_window_and_old_corruption_regression(self):
        v = read_layout()
        validate_bounce(v['BOUNCE_END'])
        # Both historical failures must be rejected: stack at 0x7F000 and
        # BIOS E820/VBE replies at 0x80000..0x82100, inside a legal payload.
        for name, old in [('STACK_TOP', 0x7f000), ('E820_BUF', 0x80000),
                          ('VBINFO', 0x81000), ('MODEINFO', 0x82000)]:
            broken = dict(v, **{name: old})
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'overlaps'):
                validate_bounce(v['BOUNCE_END'], broken)
        with self.assertRaises(ValueError):
            validate_bounce(v['BOUNCE_END'] + 4096)

    def test_full_payload_preserves_firmware_and_stack(self):
        v = read_layout()
        ram = bytearray(0xa0000)
        regions = reserved_ranges()
        for _, lo, hi in regions:
            ram[lo:hi] = b'\x5a' * (hi-lo)
        ram[v['BOUNCE']:v['BOUNCE_END']] = b'\xa5' * (v['BOUNCE_END']-v['BOUNCE'])
        for name, lo, hi in regions:
            self.assertEqual(ram[lo:hi], b'\x5a' * (hi-lo), name)
        ram[v['STACK_TOP']-16:v['STACK_TOP']] = b'\x11' * 16
        self.assertEqual(ram[v['BOUNCE']:v['BOUNCE_END']], b'\xa5' * (v['BOUNCE_END']-v['BOUNCE']))
        for segment, addr in [('E820_SEG','E820_BUF'),('VB_SEG','VBINFO'),('MI_SEG','MODEINFO')]:
            self.assertEqual(v[segment] << 4, v[addr])

    def test_actual_memory_map_instructions(self):
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
        from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EBP
        v = read_layout()
        bindings = dict(v, e820_cnt=0x500, mmap_len=0x504)
        body = (ROOT / 'boot/direct_mmap.inc').read_text()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            if shutil.which('nasm'):
                defs = '\n'.join(f'{k} equ {n}' for k,n in bindings.items())
                (work/'map.asm').write_text('bits 32\n'+defs+'\n_start:\n'+body+'\nhlt\n')
                subprocess.run(['nasm','-f','bin','-o',str(work/'map.bin'),str(work/'map.asm')],check=True)
            else:
                # Only dialect spelling changes; instructions and control flow
                # come from the loader include, not a Python reimplementation.
                body = '\n'.join(line.split(';',1)[0] for line in body.splitlines())
                for key, value in bindings.items():
                    body = re.sub(r'\b'+re.escape(key)+r'\b',hex(value),body)
                body = re.sub(r'\bdword\b','DWORD PTR',body)
                (work/'map.s').write_text('.intel_syntax noprefix\n.text\n_start:\n'+body+'\nhlt\n')
                subprocess.run(['as','--32','-o',str(work/'map.o'),str(work/'map.s')],check=True)
                subprocess.run(['objcopy','-O','binary','-j','.text',str(work/'map.o'),str(work/'map.bin')],check=True)
            code = (work/'map.bin').read_bytes()
        fixtures = [
            ([(0,0x9fc00,1),(0x100000,127*1048576,1)],127*1024),
            ([(0x100000,31*1048576,1)],31*1024),
            ([(0x100000,63*1048576,1),(0x4000000,0x100000,2)],63*1024),
            ([(0x100000000,0x40000000,1)],31744),
            ([],31744),
        ]
        for entries, expected in fixtures:
            with self.subTest(entries=entries):
                cpu = Uc(UC_ARCH_X86, UC_MODE_32)
                cpu.mem_map(0,0x100000)
                cpu.mem_write(0x1000,code)
                cpu.reg_write(UC_X86_REG_ESP,v['STACK_TOP'])
                cpu.mem_write(bindings['e820_cnt'],struct.pack('<I',len(entries)))
                raw = b''.join(struct.pack('<QQII',addr,size,kind,1) for addr,size,kind in entries)
                if raw: cpu.mem_write(v['E820_BUF'],raw)
                cpu.emu_start(0x1000,0x1000+len(code),count=20000)
                self.assertEqual(cpu.reg_read(UC_X86_REG_EBP),expected)
                self.assertEqual(struct.unpack('<I',cpu.mem_read(bindings['mmap_len'],4))[0],len(entries)*24)
                for i,(addr,size,kind) in enumerate(entries):
                    self.assertEqual(bytes(cpu.mem_read(v['MMAP_TAB']+24*i,24)),struct.pack('<IQQI',20,addr,size,kind))


if __name__ == '__main__':
    unittest.main(verbosity=2)
