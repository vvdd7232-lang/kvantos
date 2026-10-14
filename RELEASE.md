# KvantOS 3.1.0

October 14, 2026. The next release after 3.0.0 "Horizon".

## Highlights

- Dirty-rectangle GUI rendering, cached wallpaper/icons and redraws limited to intersecting windows. Moving the cursor no longer repaints the entire desktop.
- KvApp ABI 3 adds `delta_ms()` while retaining ABI 2 compatibility. A 500 Hz timer and elapsed-time simulation stabilize Pong, Snake, Life and Stars.
- **Maze 3D**, an integer-only first-person maze with ray casting, shaded walls, collisions and a minimap.
- **Eleven applications**, each built for both 32-bit and 64-bit kernels.
- BIOS/GRUB and native x86-64 UEFI boot remain supported, alongside an alternative GRUB-free CD/DVD ISO.

## Release-blocking fixes

Modern OVMF testing uncovered EFI loader problems fixed in this release: code overlapping PE headers, missing relocation metadata and BSS initialization, incorrect EFI memory type values, GOP discovery, kernel memory allocation, and boot metadata placed outside the kernel's initial identity mapping. Calling EFI console services after `ExitBootServices` has been removed.

The direct BIOS loader now uses the correct copy-table address, preserves registers, maintains the copy counter, keeps its stack away from its own code, and reads absolute 2048-byte CD sectors. The GRUB menu reports 3.1.0 and loads all eleven applications.

The main ISO includes GRUB disk bootstrap code and a partition table exposing the ESP for USB use. The direct ISO is intended for CD/DVD-style boot, not raw USB writing.

## Downloads

Full distribution: **`kvantos-3.1.0.tar.gz`**.

- `kvantos.iso`: recommended BIOS/UEFI image for virtual machines and USB.
- `kvantos-direct.iso`: alternative BIOS/UEFI CD/DVD image without GRUB.
- `kvantos-floppy.img`: 32-bit boot floppy without embedded applications.
- `kvant.bin`, `kvant64.bin`: both kernel builds.
- `kvantefi.efi`, `esp.img`: UEFI loader and ESP image.
- `apps/`, `apps64/`: eleven applications per architecture.
- `kvantos-disk.img`: empty 16 MiB KvFS data disk, not a preinstalled OS.
- `SHA256SUMS`: image, kernel, loader and application checksums.

Matching sources: `kvantos-3.1.0-source.tar.gz`. Verify separate downloads using `kvantos-3.1.0-SHA256SUMS.txt`.

## Running

Extract the distribution. For BIOS:

```sh
qemu-system-i386 -m 256 -boot d -cdrom kvantos.iso \
  -drive file=kvantos-disk.img,format=raw,if=ide
```

For UEFI, use x86-64 and OVMF:

```sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 256 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -cdrom kvantos.iso -boot d
```

At the shell, enter `guimenu` for the desktop. Select the second GRUB entry or type `lang ru` for Russian. Attach a KvFS disk to persist files; RAMFS files disappear at shutdown. Secure Boot is unsupported because the EFI loader is unsigned.

After extraction:

```sh
sha256sum -c SHA256SUMS
```

## Build and validation

The release uses GCC 12.4, NASM, GNU binutils, GRUB and xorriso. GCC 14.2 in this environment hit an internal compiler error optimizing 64-bit `kernel/main.c`; GCC 12 was used without reducing kernel optimization.

```sh
make CC=gcc-12 release
python3 tests/boot/check-images.py
bash tests/boot/qemu-smoke.sh release/kvantos.iso
bash tests/boot/qemu-smoke.sh release/kvantos-direct.iso
BOOT_MEDIA=usb bash tests/boot/qemu-smoke.sh release/kvantos.iso
python3 tests/boot/gui-smoke.py
```

Both ISOs are tested under QEMU BIOS (Pentium, 128 MiB) and UEFI (OVMF, 256 MiB). A separate main-ISO GUI check verifies eleven loaded applications, desktop entry, Maze 3D execution and return to the shell on both architectures.

## Limitations

An experimental OS, not a protected daily-use system. Applications lack full isolation and can hang the machine. NTFS is read-only. Read `INSTALL.md` before installing to a real disk: disk operations can destroy data.

This release is based on the current 3.1.0 branch. The C Workshop changes in a separate old local 32-bit branch have not been ported and are not claimed as part of 3.1.0.

License: MIT.
