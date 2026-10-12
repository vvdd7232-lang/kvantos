# Vendored dependencies

`pycdlib/` - pure-Python ISO9660 writer (LGPL-2.1, v1.20.0),
used by `tools/mkdirect.py` to build the GRUB-free bootable ISO
(`make iso-direct`). Vendored because build environments may have
no network access and no xorriso.
