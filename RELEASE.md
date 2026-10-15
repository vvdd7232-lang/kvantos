# KvantOS 3.1.2 — mouse-first editors

October 15, 2026. A separate update to Notepad, C Workshop and desktop navigation for **BIOS/i386 and UEFI/x86-64**. Older releases and assets are not replaced.

## Getting started

1. Boot **v3.1.2 `kvantos.iso`** and enter `guimenu` in the text shell.
2. Single-click **Notepad** or **C Workshop**, or choose it from the bottom-left **KVANT** menu.
3. Click text to position the caret; drag to select. **Copy / Cut / Paste / Undo / Select all** have clickable buttons.
4. In Workshop, click **Build** or **Run** to compile the built-in `hello.c` into a native application. **Help** has its own Close help button.
5. Close windows with the cross, switch with the taskbar/menu, and use **KVANT → Exit to console** to leave.

Single-letter desktop launch/close/quit commands such as **G/B/E/T/Q/W** are gone. The keyboard remains for text and optional editor shortcuts; **in-game keyboard controls are preserved**. Esc in an editor dismisses a dialog/selection, not the window or desktop.

## Changes

- Mouse caret positioning, multiline selection and edge auto-scroll. UTF-8 caret, selection and deletion respect character boundaries.
- Shared clipboard between editors; independent documents and one-step undo/redo. Click Undo again to redo.
- **Open / Save as** dialogs with RAM/Disk (KvFS) file lists, paging, a clickable filename caret, confirmation and cancel buttons. Selecting a file no longer requires typing a path or pressing Enter.
- Working draggable vertical/horizontal scrollbars with arrow/page controls; mouse wheel and Shift+wheel. Scrolling does not chase an unchanged caret during repaint.
- Unsaved-change guards on new/open/close and both desktop exit paths. Replacing another existing file or deleting requires confirmation.
- **Notepad now saves to RAM + mounted KvFS**, like Workshop. Without a supported disk RAM is temporary and the status warns about it.
- Atomic bounded editing: 128 lines, 120 bytes/line. Overflowing paste, line joins or loads never silently truncate or discard the current document. Binary input is rejected.
- Optional **MouseKeys**, enabled through KVANT or NumLock: keypad 8/2/4/6 and diagonals move, 5 clicks, 0 holds, decimal releases, Shift is precise, Ctrl faster and +/- adjust speed. Off by default.

Dirty-rectangle rendering, desktop caching, both native compiler backends, `.c/.kapp`, RAM/KvFS, eleven SDK apps per architecture and both boot paths remain intact. Editor layouts also fit **640×480**.

[Workshop guide](https://github.com/vvdd7232-lang/kvantos/blob/v3.1.2/sdk/WORKSHOP.md) · [README](https://github.com/vvdd7232-lang/kvantos/blob/v3.1.2/README.md).

![Mouse selection in Notepad](https://raw.githubusercontent.com/vvdd7232-lang/kvantos/v3.1.2/screenshots/63_notepad_mouse_uefi64.png)

![Mouse-first Workshop](https://raw.githubusercontent.com/vvdd7232-lang/kvantos/v3.1.2/screenshots/64_workshop_mouse_uefi64.png)

## Validation

Built with GCC 12. The release workflow runs these checks before publishing:

- 73 generated-code checks/backend, 40,000 compiler mutations/backend under ASan/UBSan, atomic RAMFS/OOM tests.
- Text model: selection, UTF-8, undo/redo, limits and atomic failures, plus 25,000 edits under ASan/UBSan.
- Both QEMU kernels: actual mouse editing, clipboard, file dialogs, save/cancel/overwrite, empty/binary/oversize files, wheel and both thumbs, edge auto-scroll, file paging, MouseKeys, Russian UI, exit guards and 640×480 bounds.
- Actual Notepad KvFS persistence, inspecting disk content after the guest exits.
- In-guest C compilation/execution, independent editors, native draw/click, no stale run after errors, source reopening after a full reboot, ABI/heap/eight arguments/variadic format and Russian diskless build/run.
- BIOS/UEFI CD and USB, GRUB-free direct ISO and native Maze 3D keyboard/timer regressions.

Pointer tests use measured guest coordinates and bounded movement, not blind mouse retries. Physical hardware is not separately certified.

## Downloads and running

- **`kvantos.iso`**: recommended BIOS/UEFI ISO and raw-USB hybrid.
- **`kvantos-direct.iso`**: GRUB-free BIOS/UEFI CD/DVD fallback, not a raw-USB hybrid.
- **`kvantos-3.1.2.tar.gz`**: full distribution with both ISOs, floppy, kernels, EFI, ESP, empty KvFS disk, apps, docs and SHA256SUMS.
- **`kvantos-3.1.2-source.tar.gz`**: matching sources; **`kvantos-3.1.2-SHA256SUMS.txt`**: download checksums.

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

Persistence requires supported **IDE/ATA PIO**, not AHCI/SATA/NVMe. q35's default `-drive if=ide` is unsupported AHCI; use the separate PIIX3 controller above. Diskless compilation and execution still work in RAM. Use `lang ru` for Russian. Inside the full distribution run `sha256sum -c SHA256SUMS`; for release downloads use `sha256sum -c kvantos-3.1.2-SHA256SUMS.txt`.

## Limitations

Experimental OS: apps run in **ring 0 without full isolation** and can corrupt memory or hang the machine. Workshop implements a limited C subset, not GCC/full ISO C: no preprocessing, structures, typedefs, floating point, libc, array declarations or complete type checking. Source can be moved between architectures and rebuilt; generated binaries are architecture-specific. Clipboard is internal, not shared with the host OS; undo is one-step. A Russian keyboard layout and direct editor writes to FAT32/NTFS were not added in this release.

Back up data and read INSTALL.md before installing to a real disk. License: MIT.
