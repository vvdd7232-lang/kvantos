# KvantOS 3.1.3 — USB import, GPT and non-blocking line warnings

October 16, 2026. Separate **BIOS/i386 and UEFI/x86-64** release;
previous release tags and assets are not overwritten.

## Changes

* Read-only USB mass-storage import in Notepad and C Workshop: mouse folder/file
  picker, Refresh, editable local name and overwrite/unsaved-change guards.
  No PC-side KvFS image is needed for source transfer.
* UHCI and high-speed EHCI, FAT16/FAT32 on MBR/raw media and **GPT + FAT32**,
  including an MSR before the data partition. Bounded GPT header/array CRC
  verification and backup fallback; no format, conversion, repair or writes.
* **128 lines is a warning only**, not an editor ceiling. Import/edit/clipboard,
  undo/redo/save and native Build/Run continue within their separate byte limits.
  Packed text, live-prefix copies, cached width and five-digit line gutters.
* Kept mouse-first editors, optional NumPad MouseKeys, independent documents,
  RAM/KvFS persistence, both native compiler backends and all 11 apps/backend.

## Getting started

Boot the new `kvantos.iso`, enter `guimenu`, open C Workshop or Notepad.
In VirtualBox select **USB 2.0 (OHCI + EHCI)** and capture the drive.
Use **Import → Refresh → folder → file → Import**, then Save/Build/Run.
There is no need to reformat or convert GPT to MBR. Boot the new ISO explicitly;
changing the ISO does not update an older installed kernel automatically.

Import copies into RAM; only Save/Build persists to mounted KvFS. RAM is lost
on reboot without KvFS. FAT32 remains read-only.

## Limits

Editor input and normalized text **<64 KiB**, **120 bytes/line**, no independent
row-count ceiling. Native compiler source remains **<=16000 bytes**; larger
text can be imported, edited and saved, while Build reports the limit without
changing text/undo. This is still a limited C subset, not GCC/C99.
UTF-8/BOM and CRLF work; tabs become four spaces; binary/UTF-16/overflow is
rejected atomically. Picker: 128 matches from the first 512 directory entries.

EHCI high-speed root-port BOT and UHCI only; no standalone OHCI driver,
xHCI/USB 3, hubs, split transactions or UAS. VirtualBox USB 1.1 is OHCI, not
UHCI. Current block layer uses 512-byte sectors and 32-bit sector addresses;
GPT is bounded to 1024 entries and is never repaired or written.

[USB guide](https://github.com/vvdd7232-lang/kvantos/blob/v3.1.3/USB-IMPORT.md)
· [Workshop guide](https://github.com/vvdd7232-lang/kvantos/blob/v3.1.3/sdk/WORKSHOP.md).

![517 lines: successful build, non-blocking warning](https://raw.githubusercontent.com/vvdd7232-lang/kvantos/v3.1.3/screenshots/68_workshop_lines_warning_uefi64.png)

## Validation

Release workflow runs GCC 12 builds; 73 code-generation checks/backend;
40000 sanitizer mutations/backend; RAMFS/OOM; ASan/UBSan text model with
25000 edits, full byte pool, 65536 empty rows, undo/redo and atomic Tab overflow.
The empty-row maximum follows from the byte budget, not a new separate row cap.

23 GPT/MBR/raw checks cover CRCs, backup fallback and bounds. On both kernels,
USB 2.0/EHCI with OHCI companions is tested on GPT and MBR: folders, BOM/CRLF,
rename, import/edit/build/run, 12 rescans, unplug/replug, KvFS persistence and
full reboot. Source USB SHA-256 hashes must remain unchanged.

A 517-line source (512 comment lines plus a working app) is edited beyond row
128, undone/redone with the mouse, built/run and reopened/run after reboot.
An >16000-byte source may be edited/saved but is refused clearly by Build,
without changing text/undo. Notepad uses independent 2049- and 10001-line
UTF-8 documents, persistence and five-digit gutters.

Mouse/clipboard/dialog/scroll/wheel/MouseKeys/Russian/640x480 regressions,
desktop/Maze 3D, native ABI/heap, BIOS/UEFI CD, firmware USB and direct ISO
checks are retained. Tests run in **QEMU**, with measured pointer coordinates;
physical hardware and VirtualBox itself have not been independently tested.

## Downloads and running

- **`kvantos.iso`**: recommended BIOS/UEFI ISO and raw-USB hybrid.
- **`kvantos-direct.iso`**: GRUB-free BIOS/UEFI CD/DVD fallback, not a raw-USB hybrid.
- **`kvantos-3.1.3.tar.gz`**: full distribution with both ISOs, floppy, kernels, EFI, ESP, empty KvFS disk, apps, docs and SHA256SUMS.
- **`kvantos-3.1.3-source.tar.gz`**: matching sources; **`kvantos-3.1.3-SHA256SUMS.txt`**: download checksums.

Extract the full distribution. BIOS example:

```sh
qemu-system-i386 -m 256 -boot d -cdrom kvantos.iso \
  -drive file=kvantos-disk.img,format=raw,if=ide
```

UEFI example (OVMF, Secure Boot disabled):

```sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 256 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -cdrom kvantos.iso -boot d \
  -device piix3-ide,id=kvide \
  -drive file=kvantos-disk.img,format=raw,if=none,id=kvdata \
  -device ide-hd,bus=kvide.0,drive=kvdata
```

Persistence requires supported **IDE/ATA PIO**, not AHCI/SATA/NVMe. q35's default `-drive if=ide` is unsupported AHCI; use the separate PIIX3 controller above. Diskless compilation and execution still work in RAM. Use `lang ru` for Russian. Inside the full distribution run `sha256sum -c SHA256SUMS`; for release downloads use `sha256sum -c kvantos-3.1.3-SHA256SUMS.txt`.

## Limitations

Experimental OS: apps run in **ring 0 without full isolation** and can corrupt memory or hang the machine. Workshop implements a limited C subset, not GCC/full ISO C: no preprocessing, structures, typedefs, floating point, libc, array declarations or complete type checking. Source can be moved between architectures and rebuilt; generated binaries are architecture-specific. Clipboard is internal, not shared with the host OS; undo is one-step. A Russian keyboard layout and direct editor writes to FAT32/NTFS were not added in this release.

Back up data and read INSTALL.md before installing to a real disk. License: MIT.
