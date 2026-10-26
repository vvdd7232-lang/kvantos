"""Direct-loader low-memory contract, checked before emitting a boot image."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read_layout():
    source = (ROOT / 'boot/direct.asm').read_text()
    values = {name: int(value, 0) for name, value in re.findall(
        r'^([A-Z][A-Z0-9_]*)\s+equ\s+(0x[0-9A-Fa-f]+|[0-9]+)\b', source, re.M)}
    # The protected-mode copy stack must obey the same reservation as real mode.
    if not re.search(r'\bmov\s+esp,\s*STACK_TOP\b', source):
        raise ValueError('direct protected-mode stack must use STACK_TOP')
    return values


def reserved_ranges(layout=None):
    v = read_layout() if layout is None else layout
    return [
        ('stack', v['STACK_TOP'] - 4096, v['STACK_TOP']),
        ('loader/tables', 0x7c00, 0x8400),
        ('disk packet', v['DAP'], v['DAP'] + 16),
        ('E820', v['E820_BUF'], v['E820_BUF'] + v['E820_MAX'] * 24),
        ('VBE controller', v['VBINFO'], v['VBINFO'] + 512),
        ('VBE mode', v['MODEINFO'], v['MODEINFO'] + 256),
        ('Multiboot info', v['MBI'], v['MBI'] + 128),
        ('module table', v['MODS_TAB'], v['MODS_TAB'] + 16 * 16),
        ('Multiboot mmap', v['MMAP_TAB'], v['MMAP_TAB'] + v['E820_MAX'] * 24),
        ('boot strings', v['STR_AREA'], v['STR_AREA'] + 4096),
    ]


def validate_bounce(end, layout=None):
    v = read_layout() if layout is None else layout
    start = v['BOUNCE']
    if not start <= end <= v['BOUNCE_END']:
        raise ValueError(f'direct bounce end {end:#x} outside {start:#x}..{v["BOUNCE_END"]:#x}')
    regions = reserved_ranges(v)
    for name, lo, hi in regions:
        if lo < end and start < hi:
            raise ValueError(f'direct bounce {start:#x}..{end:#x} overlaps {name} {lo:#x}..{hi:#x}')
    for index, (name, lo, hi) in enumerate(regions):
        for other, begin, stop in regions[index + 1:]:
            if lo < stop and begin < hi:
                raise ValueError(f'direct reserved areas overlap: {name} and {other}')
