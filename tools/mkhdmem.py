#!/usr/bin/env python3
"""Small deterministic GRUB memdisk: no redundant on-disk .mod copies.
For the installed BIOS loader or the bare floppy fallback. gzio is linked into core; the kernel
inside the tar is gzip-compressed, while the ISO/UEFI kernels stay plain ELF.
"""
import io
import sys
import tarfile
from pathlib import Path

out, config, kernel = map(Path, sys.argv[1:4])
apps = Path(sys.argv[4]) if len(sys.argv) > 4 else None
with tarfile.open(out, 'w', format=tarfile.USTAR_FORMAT) as archive:
    files = [('boot/grub/grub.cfg', config), ('boot/kvant.bin', kernel)]
    if apps is not None:
        files += [('boot/apps/' + p.name, p) for p in sorted(apps.glob('*.kapp'))]
    for name, path in files:
        data = path.read_bytes()
        info = tarfile.TarInfo(name)
        info.size, info.mode, info.mtime = len(data), 0o644, 0
        archive.addfile(info, io.BytesIO(data))
