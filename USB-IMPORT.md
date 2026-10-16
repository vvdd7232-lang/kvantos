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

Read-only **UHCI full-speed and EHCI high-speed** root ports, mass storage
**BOT / SCSI transparent / LUN 0 / 512-byte sectors**. Up to four controllers
and eight disks. **No OHCI, xHCI, hubs, split transactions, UAS, or 4K sectors.**
EHCI MMIO must be below 4 GiB; failed firmware handoff is reported, not forced.
No USB HID driver; keep PS/2 input or MouseKeys available.

## VirtualBox USB 2.0 correction

The old UHCI-only preview cannot work with VirtualBox's USB controllers.
Use the new `kvantos-usb2-ehci-preview.iso`, select **USB 2.0 (OHCI + EHCI)**,
and check actual device capture under **Devices > USB** after boot. A filter
alone does not prove capture. Click Import > Refresh; the status reports EHCI
controller and detected disk counts. Do not reformat a device just to diagnose
an empty list. USB 1.1 (OHCI) and USB 3.0 (xHCI) remain unsupported.

High-speed EHCI has been validated under **QEMU**, with ICH4 and ICH9 models,
including OHCI companions and root port 3 at 480 Mb/s. **VirtualBox itself,
the user's physical flash drive, and real hardware have not been tested.**
OHCI companions do not imply an OHCI driver: full-/low-speed devices on them
remain unsupported. Firmware USB boot is separate from OS USB data support.

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
USB_CONTROLLER=ehci python3 tests/boot/usb-import-smoke.py
USB_CONTROLLER=ehci-ohci python3 tests/boot/usb-import-smoke.py
USB_CONTROLLER=uhci python3 tests/boot/usb-import-smoke.py
python3 tests/boot/mkusb.py build/usb-fixtures
make -C tests/fs
tests/fs/build/harness build/usb-fixtures/flash-raw.img --usb
tests/fs/build/harness build/usb-fixtures/flash-mbr.img --usb
```

Needs dosfstools, mtools, QEMU, OVMF and Pillow. Only private files under
`build/` are created, no host mounts or host-device formatting. Guest tests
cover BIOS/i386 and UEFI/x86-64, MBR/raw USB FAT32, folders/LFN, BOM/CRLF/tabs,
Unicode text, naming and overwrite guards, 6 KiB multi-cluster sources, empty files, atomic bad-input
rejection, 12 refreshes, unplug/replug, native compilation and execution,
KvFS persistence over a full reboot, diskless operation and Russian 640x480.
The QEMU USB images are writable; their SHA-256 hashes must remain unchanged.
Host routing tests additionally exercise write refusal, read bounds and 24
mount resets. This is emulated-device validation, not real-hardware coverage.

Attach an existing FAT32 image with these extra QEMU options:

```sh
-device usb-ehci,id=usbtest \
-drive if=none,id=flash,format=raw,file=flash.img \
-device usb-storage,bus=usbtest.0,drive=flash,port=1
```
