# FAT32 / USB import — development preview after 3.1.2

This feature belongs to `feature/fat32-import`, **not the published 3.1.2 ISO**.
Existing release files and tags are unchanged. [Russian instructions](USB-IMPORT.ru.md).

Click **Import** in C Workshop or Notepad, **Refresh USB** after plugging in,
and **Next volume** to cycle FAT volumes. Click folders, **Up** to return,
select a file, optionally edit its separate local name, then **Import**.
The source remains untouched. The validated copy opens in RAM; **Save** or
**Build/Run** persists it to a mounted KvFS. Without KvFS it is temporary.
Import itself does not overwrite a KvFS file. Name collisions require
confirmation; cancellation/read/validation/OOM failures preserve doc and undo.

## Deliberately limited hardware support

Read-only **UHCI USB 1.1**, full-speed root ports, mass storage **BOT / SCSI
transparent / LUN 0 / 512-byte sectors**. Up to four controllers, two root
ports each. **No EHCI, xHCI, OHCI, hubs, UAS, or 4K logical sectors.** Most
modern xHCI-only PCs will not detect a flash drive with this driver. A USB 2
connector or firmware USB boot capability does not imply UHCI support.
No physical hardware has been tested. Taking UHCI ownership disables its
legacy SMI; USB keyboard/mouse BIOS emulation may cease working (no USB HID
driver). PS/2 and existing NumLock/NumPad MouseKeys remain supported.

FAT16/FAT32 in **MBR** primary/extended partitions or raw/superfloppy form.
No new GPT/exFAT/FAT12 support. ATA FAT volumes are also available in the
picker; USB tests use a real emulated USB controller, never an IDE substitute.
USB writes are independently rejected by both the block layer and VFS.
The existing file manager can copy other file types to local storage (4 MiB
limit), subject to destination filesystem name/capacity limits.

UTF-8 (optional BOM), CRLF normalization, tabs to four spaces. Empty files
work; binary, UTF-16 and overflow are rejected. Editor: 128 lines, 120 bytes
per line, input <15488 bytes; local name <=23 ASCII characters (Workshop:
18-character stem + `.c`). Picker: 128 matching items from the first 512
entries, with a truncation warning; VFS paths <=255 bytes, names <=127.
Directory entries are cached, not reread each frame. Refresh invalidates
both pickers and the read-only sector cache and starts at the first FAT root.

## Reproducible tests

```sh
make CC=gcc-12 iso
python3 tests/boot/usb-import-smoke.py
python3 tests/boot/mkusb.py build/usb-fixtures
make -C tests/fs
tests/fs/build/harness build/usb-fixtures/flash-raw.img --usb
tests/fs/build/harness build/usb-fixtures/flash-mbr.img --usb
```

Needs dosfstools, mtools, QEMU, OVMF and Pillow. Only private files under
`build/` are created, no host mounts or host-device formatting. Guest tests
cover BIOS/i386 and UEFI/x86-64, MBR/raw USB FAT32, folders/LFN, BOM/CRLF/tabs,
Unicode text, naming and overwrite guards, empty files, atomic bad-input
rejection, 12 refreshes, unplug/replug, native compilation and execution,
KvFS persistence over a full reboot, diskless operation and Russian 640x480.
The QEMU USB images are writable; their SHA-256 hashes must remain unchanged.
Host routing tests additionally exercise write refusal, read bounds and 24
mount resets. This is emulated-device validation, not real-hardware coverage.

Attach an existing FAT32 image with these extra QEMU options:

```sh
-device piix3-usb-uhci,id=usbtest \
-drive if=none,id=flash,format=raw,file=flash.img \
-device usb-storage,bus=usbtest.0,drive=flash,port=1
```
