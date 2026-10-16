# Boot smoke tests

`qemu-smoke.sh` boots the release ISO through both supported firmware paths:

* BIOS/GRUB with a Pentium CPU model (32-bit kernel)
* UEFI/OVMF (64-bit kernel)

Each run must reach the `kvant:/$` shell prompt on COM1 within 60 seconds.
That check happens after interrupts are enabled, so the BIOS run exercises the
PIT IRQ path rather than stopping at the kernel's pre-interrupt startup banner.

```sh
make release
tests/boot/qemu-smoke.sh release/kvantos.iso
```

Requirements: `qemu-system-x86` and a matching OVMF `CODE`/`VARS` pair. The
script searches the standard Ubuntu paths; set both `OVMF_CODE` and `OVMF_VARS`
to override them. `BOOT_TIMEOUT_SECONDS` changes the per-boot timeout.


## Release regressions (3.1.0)

Use GCC 12 for this release (`make CC=gcc-12 release`). GCC 14.2 in the
release environment hit an internal compiler error in 64-bit `main.c`.

```sh
python3 tests/boot/check-images.py
bash tests/boot/qemu-smoke.sh release/kvantos.iso
bash tests/boot/qemu-smoke.sh release/kvantos-direct.iso
BOOT_MEDIA=usb bash tests/boot/qemu-smoke.sh release/kvantos.iso
python3 tests/boot/gui-smoke.py
```

The image checker validates PE headers, relocation metadata, initialized
loader globals, both El Torito entries, the main ISO MBR/GPT, direct-loader
absolute CD sector tables and payloads, application counts and checksums.
The GUI test checks all eleven RAM applications, native Maze 3D execution,
keyboard input, timer progression and shell return in both kernel builds.
It uses measured guest pointer coordinates and real PS/2 clicks, not blind
mouse aiming. Logs/screenshots are under `build/gui-smoke/`.

`BOOT_MEDIA=usb` attaches the main ISO as raw USB storage; this specifically
checks the hybrid partition/boot layout rather than falling back to CD boot.
The direct ISO is a CD/DVD fallback, not a raw-USB hybrid.

After committing/tagging `v3.1.0`, `bash tools/package-release.sh` creates the
source archive and external checksums under `packages/`. The workflow
`.github/workflows/release.yml` builds/tests/packages before publishing on a
`v3.1.0` tag push and refuses to overwrite an existing release.


## Mouse-first editors (3.1.2)

```sh
make CC=gcc-12 release
make -C tests/mcc CC=gcc-12 editors
python3 tests/boot/editor-smoke.py
```

Uses real pointer input with bounded guest-coordinate feedback, never blind
mouse retries. Checks disabled letter launchers, single-click icons and menu,
caret/selection/edge auto-scroll, cross-editor clipboard, undo/redo, UTF-8,
RAM/disk file pickers and paging, filename caret, empty/binary/oversize files,
new/close/quit/overwrite guards, wheel, both draggable scrollbars, optional
MouseKeys, 640x480 bounds, Russian UI and actual Notepad KvFS persistence.
Generated screenshots and private disks live under `build/editor-smoke/`.

## Read-only FAT32 / USB import (development after 3.1.2)

```sh
make CC=gcc-12 iso
python3 tests/boot/usb-import-smoke.py
python3 tests/boot/mkusb.py build/usb-fixtures
make -C tests/fs
tests/fs/build/harness build/usb-fixtures/flash-raw.img --usb
tests/fs/build/harness build/usb-fixtures/flash-mbr.img --usb
```

Needs dosfstools and mtools in addition to QEMU, OVMF and Pillow. Tests only
create private images, never format/mount a host device. Both real guest
kernels enumerate a `piix3-usb-uhci` + `usb-storage` device, walk MBR/raw
FAT32, navigate folders and import via actual mouse widgets. Covers BOM,
CRLF, UTF-8, tabs, long FAT names/local rename, empty files, atomic rejection,
replacement guards, independent editors, 12 refreshes, physical USB device
removal/re-addition, Build/Run, KvFS persistence across reboot, diskless mode
and Russian 640x480. Source-image hashes must remain unchanged despite
writable QEMU backends. Host tests exercise block/VFS write refusal, bounds
and 24 mount resets. Logs/screenshots: `build/usb-import/{bios32,uefi64}`.

This does **not** validate arbitrary hardware. Driver supports UHCI full-speed / EHCI high-speed root
ports and BOT/SCSI/512-byte media only; no xHCI/OHCI/hubs/UAS. See
[USB-IMPORT.md](../../USB-IMPORT.md) before trying a physical machine.

### EHCI / VirtualBox USB 2.0 correction

```sh
USB_CONTROLLER=ehci python3 tests/boot/usb-import-smoke.py
USB_CONTROLLER=ehci-ohci python3 tests/boot/usb-import-smoke.py
```

The first case uses QEMU ICH4 EHCI. The second uses ICH9 EHCI, two OHCI
companions, and root port 3. Tests assert 480 Mb/s and successful EHCI init;
no UHCI fallback or IDE substitution. The full existing USB suite runs in
both kernels, including a 6 KiB multi-cluster source. Generated evidence is
under `build/usb-import/{uhci,ehci,ehci-ohci}/{bios32,uefi64}`.
This is **not** a VirtualBox run and does not establish physical-drive
compatibility. OHCI is a companion in the test topology, not a supported
driver. VirtualBox users should use the new ISO and USB 2.0 (OHCI + EHCI),
check Devices > USB capture, and Refresh. USB 1.1 OHCI and USB 3.0 xHCI remain
unsupported. EHCI qTD/QH layout is checked at compile time; async DMA is
quiesced before descriptor reuse, registers are mapped uncached, and BIOS
ownership has a bounded handoff rather than blindly overwriting firmware.

### GPT + FAT32 (read-only)

```sh
USB_CONTROLLER=ehci-ohci USB_LAYOUT=gpt python3 tests/boot/usb-import-smoke.py
USB_CONTROLLER=uhci USB_LAYOUT=gpt python3 tests/boot/usb-import-smoke.py
make -C tests/fs gpt
```

The GPT fixture has a Microsoft Reserved partition in slot 1 and a FAT32
Basic Data partition in slot 2, primary/backup headers and arrays with CRCs.
The full mouse import/6 KiB source/build/run/persistence/hotplug suite uses
`/mnt/usb0p2`, real emulated USB, and both kernel builds. Original source
hashes must remain unchanged. Evidence is now separated by controller,
layout and architecture under `build/usb-import/`.

The 23 host checks use the actual block/partition/FAT/VFS drivers and a USB
file stub. They cover MBR/raw regressions, both CRCs and backup fallback,
64-bit address bounds, malformed metadata, partition ranges, supported
entry strides, slot 1024, 24 repeated rescans, and independent `sgdisk -v`
validation when installed. No host mounts/devices. GPT is never repaired;
invalid protective GPT cannot fall back to raw/hybrid aliases. GPT mounts
are read-only, even on ATA. Parser limits: 512-byte sectors, 32-bit LBAs,
1024 entries, 128..4096-byte power-of-two stride, 128 KiB table maximum.
