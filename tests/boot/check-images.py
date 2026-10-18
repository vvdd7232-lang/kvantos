#!/usr/bin/env python3
"""Structural regressions for the release PE/COFF loader and both ISO paths."""
from pathlib import Path
import io
import gzip
import tarfile
import os
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
sys.path.insert(0, str(ROOT / 'tools/vendor'))
import pycdlib
sys.path.insert(0, str(ROOT / 'tools'))
from mkdirect import elf_loads

pe = Path('release/kvantefi.efi').read_bytes()
assert pe[:2] == b'MZ'
header = struct.unpack_from('<I', pe, 0x3c)[0]
assert pe[header:header + 4] == b'PE\0\0'
machine, nsections = struct.unpack_from('<HH', pe, header + 4)
optional_size, flags = struct.unpack_from('<HH', pe, header + 20)
assert machine == 0x8664 and not flags & 1, 'EFI image must be relocatable x86-64'
optional = header + 24
assert struct.unpack_from('<H', pe, optional)[0] == 0x20b
assert struct.unpack_from('<H', pe, optional + 68)[0] == 10
entry = struct.unpack_from('<I', pe, optional + 16)[0]
headers_size = struct.unpack_from('<I', pe, optional + 60)[0]
reloc_rva, reloc_size = struct.unpack_from('<II', pe, optional + 112 + 5 * 8)
assert reloc_rva and reloc_size >= 8
sections = {}
for index in range(nsections):
    offset = optional + optional_size + index * 40
    name = pe[offset:offset + 8].rstrip(b'\0').decode()
    virtual_size, rva, raw_size, raw_offset = struct.unpack_from('<4I', pe, offset + 8)
    assert rva >= headers_size and raw_offset + raw_size <= len(pe)
    sections[name] = (rva, virtual_size, raw_offset, raw_size)
text_rva, text_size, _, _ = sections['.text']
assert text_rva <= entry < text_rva + text_size
assert sections['.reloc'][0] == reloc_rva
# These globals used to be orphaned .bss and omitted by objcopy.
data_rva, data_size, data_offset, _ = sections['.data']
symbols = {}
for line in subprocess.check_output(['nm', '-S', 'build/kvantefi.so'], text=True).splitlines():
    parts = line.split()
    if len(parts) == 4:
        symbols[parts[3]] = int(parts[0], 16), int(parts[1], 16)
for name in ['gBS', 'gST', 'map_buf', 'map_pages', 'reserved', 'reserved_count']:
    address, size = symbols[name]
    assert data_rva <= address and address + size <= data_rva + data_size, name
    initial = pe[data_offset + address - data_rva:data_offset + address - data_rva + size]
    assert len(initial) == size and not any(initial), name
# No-op PE relocations are valid only while all references are RIP-relative.
env = dict(os.environ, LC_ALL='C')
assert 'There are no relocations' in subprocess.check_output(
    ['readelf', '-r', 'build/kvantefi.so'], text=True, env=env)
print('EFI: headers/entry/relocation directory/zero-initialized globals PASS')

for image in ['kvantos.iso', 'kvantos-direct.iso']:
    report = subprocess.check_output(['xorriso', '-indev', 'release/' + image,
                                      '-report_el_torito', 'plain'], stderr=subprocess.STDOUT, text=True)
    assert 'BIOS' in report and 'UEFI' in report
report = subprocess.check_output(['xorriso', '-indev', 'release/kvantos.iso',
                                  '-report_system_area', 'plain'], stderr=subprocess.STDOUT, text=True)
assert 'MBR' in report and 'GPT' in report, report
print('ISO: both El Torito firmware entries; main ISO MBR/GPT PASS')

iso = pycdlib.PyCdlib()
iso.open('release/kvantos-direct.iso')
record = iso.get_record(iso_path='/BOOT/DIRECT.IMG;1')
base_lba = record.extent_location()
stream = io.BytesIO()
iso.get_file_from_iso_fp(stream, iso_path='/BOOT/DIRECT.IMG;1')
direct = stream.getvalue()
iso.close()
count = struct.unpack_from('<I', direct, 1536)[0]
assert count == struct.unpack_from('<I', direct, 1792)[0]
assert 1536 + 4 + count * 12 <= 1792 and 1792 + 4 + count * 12 <= 2048
_, loads, _, bss_end = elf_loads(Path('release/kvant.bin').read_bytes())
expected = [blob for _, blob in loads]
expected += [p.read_bytes() for p in sorted(Path('release/apps').glob('*.kapp'))]
assert count == len(expected)
for i, blob in enumerate(expected):
    lba, sectors, bounce = struct.unpack_from('<3I', direct, 1540 + i * 12)
    source, destination, size = struct.unpack_from('<3I', direct, 1796 + i * 12)
    offset = (lba - base_lba) * 2048
    if i >= len(loads): assert destination >= bss_end, "module overlaps kernel BSS"
    assert source == bounce and size == len(blob) and sectors * 2048 >= size
    assert offset >= 2048 and offset + sectors * 2048 <= len(direct)
    assert direct[offset:offset + size] == blob
print('Direct loader: absolute CD LBAs + table bounds + kernel/app payloads PASS')
assert len(list(Path('release/apps').glob('*.kapp'))) == 11
assert len(list(Path('release/apps64').glob('*.kapp'))) == 11
subprocess.run(['sha256sum', '-c', 'SHA256SUMS'], cwd='release', check=True,
               stdout=subprocess.DEVNULL)
print('Release: 11 apps per architecture + all SHA-256 checksums PASS')

# The standalone BIOS installer no longer duplicates loadable .mod files.
# Its memdisk contains exactly the menu, a gzip ELF, and the 11 applications.
if Path('build/hd-memdisk.tar').exists():
    with tarfile.open('build/hd-memdisk.tar') as archive:
        names=archive.getnames()
        assert len(names)==13 and not any(n.endswith('.mod') for n in names)
        kernel=gzip.decompress(archive.extractfile('boot/kvant.bin').read())
        assert kernel[:4]==b'\x7fELF'
        assert elf_loads(kernel)[1]==elf_loads(Path('release/kvant.bin').read_bytes())[1]
        config=archive.extractfile('boot/grub/grub.cfg').read().decode()
        assert 'hdboot.img' not in config and 'search ' not in config
        assert config.count('menuentry ')==Path('grub/grub.cfg').read_text().count('menuentry ')
        for path in Path('release/apps').glob('*.kapp'):
            assert archive.extractfile('boot/apps/'+path.name).read()==path.read_bytes()
    assert Path('build/hd_core.img').stat().st_size<=0x78000
    print('Installed BIOS: bounded core, gzip ELF load segments, menus and all app payloads PASS')

# The bare floppy has no apps, but keeps all three menus and the same ELF
# load segments. Like the installer, its prelinked core must fit GRUB's cap.
if Path('build/fd-memdisk.tar').exists():
    with tarfile.open('build/fd-memdisk.tar') as archive:
        assert set(archive.getnames()) == {'boot/grub/grub.cfg', 'boot/kvant.bin'}
        kernel = gzip.decompress(archive.extractfile('boot/kvant.bin').read())
        assert elf_loads(kernel)[1] == elf_loads(Path('release/kvant.bin').read_bytes())[1]
        config = archive.extractfile('boot/grub/grub.cfg').read().decode()
        assert config.count('menuentry ') == 3 and 'text safe' in config
    assert Path('build/fd_core.img').stat().st_size <= 0x78000
    floppy = Path('release/kvantos-floppy.img').read_bytes()
    assert len(floppy) == 1474560 and floppy[510:512] == b'\x55\xaa'
    assert floppy[512:512+Path('build/fd_core.img').stat().st_size] == Path('build/fd_core.img').read_bytes()
    print('Floppy BIOS: bounded prelinked core, gzip ELF, three menus + boot signature PASS')
