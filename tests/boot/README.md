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

After committing/tagging `v3.3.0`, `bash tools/package-release.sh` creates the
source archive and external checksums under `packages/`. The workflow
`.github/workflows/release.yml` builds/tests/packages before publishing on a
`v3.3.0` tag push and refuses to overwrite an existing release.


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

## Read-only FAT32 / USB import (3.1.3)

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


### 128-line warning (not a ceiling)

The USB suite now accepts the old 129-line rejection fixture, imports a
512-comment-line C application, edits beyond row 128, uses mouse undo/redo,
builds/runs natively, saves to KvFS and reopens/runs after a full reboot.
A 2049-line UTF-8 Notepad document is also saved/reopened independently.
A 10001-line document exercises editing/undo/save and five-digit gutters.
An >16000-byte C document imports/edits/saves, but Build reports the separate
compiler limit without modifying text/undo. Invalid binary/UTF-16, 121-byte
lines and >=64-KiB input still fail atomically; USB hashes stay unchanged.
The host ASan/UBSan model test additionally covers the full 65535-byte pool,
65536 empty rows, offset round-trips, byte/Tab expansion overflow, undo/redo
across 128 rows, and 25000 randomized edits. The maximum empty-row case is
not an extra line cap: it follows from the byte pool and one byte per newline.
`text_model.py` shares the guest layout with both Python GUI helpers.
Historical RELEASE/AUDIT descriptions refer to their original tagged builds.


## Editor history development (3.1.4-dev)

`editor-smoke.py` now checks separate Undo (widget 77) and Redo (100), multiple
steps, Ctrl+Z/Y, saved/dirty checkpoint restoration, a new edit after Undo,
independent Workshop/Notepad histories, reset on open and actual KvFS bytes.
The host model covers the 64-entry/128-KiB budgets, full-document exchanges,
eviction, failed edits preserving redo, revision rollover and 25000 edits.
The shared ctypes history layout is in `text_model.py`.

PR workflow additionally installs QEMU/OVMF and runs the model/RAMFS sanitizers,
image validation and BIOS/UEFI main-ISO boot. Running these commands locally
is not evidence of a successful remote Actions run; the development branch
has not been pushed or published.


### M1.2: Russian input, find/replace and truthful storage status

```
make -C tests/mcc CC=gcc-12 input editors ramfs
python3 tests/boot/editor-input-smoke.py
USB_CONTROLLER=ehci-ohci USB_LAYOUT=gpt python3 tests/boot/usb-import-smoke.py
```

Input smoke needs Pillow in addition to QEMU/OVMF. Each script runs both kernels
unless `MCC_ARCH64=0` / `1` is set. It uses measured widget input, disposable image
files only and actual scan-code delivery, not guest-memory injection. EN/RU,
Alt+Shift, Caps/Shift, ё/Ё/№, Ctrl+H versus Ctrl+Backspace, physical filename/shell
compatibility, UTF-8 fields, forward/backward/wrap, one/all replacement, Undo/Redo,
independent editors, clipboard, diskless/full-directory saves, 640x480 atomic
line-limit failure and reboot/reopen are covered. BIOS additionally installs with
`setup --yes --keep` on its private disk and reboots without the CD, validating
the embedded installed GRUB path. No physical disk is touched. No-disk and disk-full
paths must not claim successful persistent storage. USB smoke covers explicit
Keep: KvFS, its no-disk refusal, deferred writes and unchanged source hashes.

These tests do not prove KvFS crash consistency, real USB hardware, or VirtualBox
compatibility. Normal-kernel FAT/USB remains read-only. New PR workflow is local configuration,
not evidence of a completed remote Actions run.


### M2.1 storage groundwork (not FAT32 save completion)

See [storage status and reproduction](../../docs/STORAGE-M2.md).
`make -C tests/mcc CC=gcc-12 storage` runs ASan/UBSan BOT/COW tests. The normal
image also supports `python3 tests/boot/kvfs-write-smoke.py`: injected backend
flush/metadata errors, GUI dirty/history preservation, mount fencing and old-file
readback after reboot. `STORAGE_LAB=1` is only for explicitly marked disposable images: transport
scratch or the FAT-file lab below. Release packaging refuses that mode; PR lab
jobs retain logs only. Production USB/FAT writes and editor destination selection
are not enabled. GUI test clicks await actual press/release acknowledgement,
not an assumed 100 ms pulse. Each click is sent once at measured coordinates;
there are no blind retry clicks.

### M2.2b actual FAT-file/recovery lab (not physical USB support)

`make CC=gcc-12 STORAGE_LAB=1 iso` creates a deliberately non-installable image.
Run `python3 tests/boot/fat-write-smoke.py`, then repeat with
`USB_CONTROLLER=uhci USB_LAYOUT=mbr`. Both architectures run by default.
The script owns regular-file MBR/GPT images, a CRC-bound manifest and preformatted
external WAL; recovery must finish before the namespace is registered in VFS.
It tests cold guest cuts, backend EIO/replay failure, actual unplug/replug, stale
mount generations, full/RO media, independent namespace/fsck and unchanged GPT.
`FAT_LAB_GENERATION_ONLY=1` selects just the fresh-grant/stale-mount regression;
`FAT_LAB_QUICK=1` is a development subset, not the full acceptance matrix.
Normal kernels refuse marked experimental volumes and keep unmarked USB RO.
Always restore `make CC=gcc-12 STORAGE_LAB=0 release` after laboratory testing.
Only logs may be published from lab builds. See [boundaries](../../docs/STORAGE-M2.md).

The prepared CI lab matrix separates EHCI/GPT from UHCI/MBR (both kernels in
each job, 25-minute limit); it retains transcripts, not experimental images.
Local passes do not imply a remote Actions run.

### M2.3a shared editor Save As

`python3 tests/boot/save-as-smoke.py` checks normal RAM/KvFS-only destinations,
replace/cancel, bound Save, Undo/Redo, both editors, Build, stale rescan refusal,
cold KvFS readback, full-directory failure without RAM fallback, and RO USB
hash preservation, including diskless 640x480. Both kernels run by default.
On the unpublishable lab ISO, `GUI_SAVE_LAB=1` tests real GUI FAT writes,
65535-byte text (>128 lines), folders, Russian LFN, cold GUI readback and USB
unplug after an observed directory-publication cut, followed by cold recovery
and independent namespace/fsck/outside-range checks. Repeat for UHCI/MBR.
`make -C tests/mcc CC=gcc-12 vfs` runs native adapter contract sanitizers.
No physical-media support or remote Actions result is implied.


## Normal owned-file FAT WAL (M2.4)

`make CC=gcc-12 STORAGE_LAB=0 iso`, then:

```sh
USB_CONTROLLER=ehci-ohci USB_LAYOUT=gpt python3 tests/boot/fat-owned-smoke.py
GUI_SAVE_OWNED=1 USB_CONTROLLER=ehci-ohci USB_LAYOUT=gpt python3 tests/boot/save-as-smoke.py
# Repeat with USB_CONTROLLER=uhci USB_LAYOUT=mbr.
```

Both commands run BIOS32 + UEFI64 by default. Use `MCC_ARCH64=0|1` for one kernel.
Private regular-file images only, never mounts/raw host devices. `mkfatowned.py`
uses the real staged provisioner, no lab manifest/spare extent. The normal kernel
must have no lab pause/test-journal symbols. `qemu_break.py` uses QEMU's Unix RSP
breakpoints at ordinary function entries, not patched kernel RAM or storage hooks.
The GUI suite performs real device_del during an uncommitted normal save and
checks the document/Undo/binding before cold recovery; the console suite checks
power cuts, EIO/RO/replay errors, reconnect fresh opt-in, full media and fsck.
Artifacts live under excluded `build/fat-owned-guest` and `build/save-as/owned`;
CI uploads logs/screenshots only. No physical qualification is implied.

Полная `make release` также проверяет запасной floppy: `qemu-smoke.sh` при
наличии `release/kvantos-floppy.img` загружает его именно через контроллер FDC.
`check-images.py` проверяет лимит core, три меню, boot signature и совпадение
ELF load segments после распаковки gzip. ISO-only сборки не требуют floppy.

### M3.1 AHCI (normal read-only kernels)

`python3 tests/boot/ahci-smoke.py` checks actual ICH9 AHCI DMA, raw/MBR/GPT,
SATA+IDE coexistence, VFS byte equality, both GUI imports, default RO,
read EIO/fencing/cold restart and source integrity. M3.2 updates the former
write/pending-WAL refusal checks as described below.
`python3 tests/boot/ahci-usb-smoke.py` checks SATA/USB coexistence with independent
WAL create/overwrite/cold/fsck for UHCI and EHCI, both kernels. Both use private
regular-file images under `build/`; no real SATA or VBox qualification.
See `docs/STORAGE-M3.md`; model checks: `make -C tests/mcc ahci`.

### M3.2 ordinary AHCI WAL writes

Set `STORAGE_CONTROLLER=ahci STORAGE_LAYOUT=mbr|gpt` for `fat-owned-smoke.py`,
and add `GUI_SAVE_OWNED=1` for `save-as-smoke.py`. Both architectures run by
default. No new kernel instrumentation: power kills at normal STX symbols,
real blkdebug write/flush/read errors, full volume, cold recovery and fsck.
AHCI QEMU IDE bus rejects hotplug, so GUI failure uses a real suspended host
flush at commit + bounded device timeout/late completion instead. A valid
late-committed NEW view is not mistaken for false save success: text and Undo
remain dirty; after reboot the entire expected old OR committed-new namespace
is checked according to the CRC-valid WAL header, then external fsck.
`ahci-usb-smoke.py` now checks dual SATA/USB WAL writes, rescan RO, cold/warm
restarts. `ahci-smoke.py` keeps read-only source checks via enable/disable and
checks recovery of an empty valid pending transaction. See STORAGE-M3.2.md.

### M3.3 OHCI USB 1.1

`USB_CONTROLLER=ohci USB_LAYOUT=mbr usb-import-smoke.py` explicitly checks native
OHCI enumeration + actual 12 Mb/s, read-only hashes, GUI import and native apps.
`STORAGE_CONTROLLER=ohci STORAGE_LAYOUT=mbr|gpt fat-owned-smoke.py` reuses the
normal owned-WAL power-cut/EIO/RO/full/unplug matrix; `GUI_SAVE_OWNED=1` with
`save-as-smoke.py` checks real GUI saves/unplug retention. Both kernels by default.
`ahci-usb-smoke.py` now includes OHCI/GPT + SATA, alongside its EHCI and UHCI cells;
`USB_CONTROLLER=ohci` selects just this new mixed-bus pair for debugging.
See `docs/STORAGE-M3.3.md`. Host backend model: `make -C tests/mcc ohci`.
