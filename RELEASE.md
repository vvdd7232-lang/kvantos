# KvantOS 3.1.4 — controlled writes, SATA/OHCI and stricter read-only NTFS

October 18, 2026. Release for **BIOS/i386** and **UEFI/x86-64**. Existing 3.1.3
tags and downloads remain unchanged.

## At a glance

3.1.4 combines editor and C Workshop updates, tightly gated FAT32 writes through
an explicitly provisioned owned-file journal, AHCI SATA and OHCI USB backends,
and additional fail-closed NTFS read bounds. Disks do not become writable by
default: opt-in requires a prepared FAT32 volume, a valid owned WAL, verified
flush capability and a separate per-session enable. NTFS remains read-only.

## Changes since 3.1.3

### Editors and C Workshop

- EN/RU keyboard input, Alt+Shift and visible layout state; literal UTF-8
  forward/backward search with wrap, Replace One and atomic Replace All as one
  undoable operation.
- USB import remains a copy into RAM. Explicit RAM/KvFS persistence state and a
  post-import KvFS offer clarify when a document will survive reboot; no
  automatic writes occur.
- Both editors share a mouse-first Save As browser: choose volume/folder, confirm
  replacement and bind subsequent saves to the destination. Namespace/device
  generation changes must not silently redirect a save.
- Read-only/error status and old-file preservation are explicit. Shell copy and
  file manager check exact byte counts, namespace generations and no-replace
  behavior; I/O failure is not silently treated as EOF.
- The bounded native C subset adds fixed one-dimensional local/global arrays
  (including pointer elements), decay/indexing and unevaluated `sizeof`.
  Existing source/data/stack budgets remain; no VLAs, array initializers, full
  ISO C or runtime index checking are claimed.
- The BIOS floppy image uses a compact GRUB memdisk and stays within the core
  limit; the separate direct BIOS/UEFI ISO remains available.

### Storage and write policy

- **All ordinary FAT volumes mount read-only.** FAT16, unprepared FAT32, unknown
  or damaged WALs never become writable automatically.
- Writes to prepared FAT32 use the exclusive owned-file `KVANTWAL.SYS` format,
  bound to validated geometry and volume identity. Valid recovery happens
  **before namespace publication**; then the operator separately enables the
  volume for the current session with `fatwrite VOLUME enable`. Disable, rescan
  or reboot removes the grant. See [docs/STORAGE-M2.md](docs/STORAGE-M2.md);
  the kernel never creates or resets the journal.
- USB BOT includes UHCI, EHCI with OHCI companions and a separate full-speed OHCI
  backend. AHCI/SATA supports reads; SATA WRITE/FLUSH EXT is gated by the shared
  generation-checked lease and owned-WAL path. Missing FLUSH support, I/O errors,
  timeouts and uncertain completions fail closed.
- GPT is read-only: header/array CRCs, bounds and backup fallback are checked.
  GPT, the MSR and gaps are not journal space. There is no automatic format,
  MBR/GPT conversion or repair.

### NTFS: read-only

- Private ntfs-3g image coverage uses logical sectors of 512/1024/2048/4096 bytes,
  resident/nonresident `$DATA`, Unicode names, MBR start offsets, partial reads,
  EOF and a fragmented stream.
- Reads zero-fill exactly at NTFS Initialized Size, including a boundary inside a
  cluster. Poisoned `0xA5` suffix bytes are not returned.
- BPB total sectors with a nonzero high DWORD and `$DATA` sizes beyond the 32-bit
  VFS range are refused, not truncated to low-word aliases. **Files above 4 GiB
  and volumes beyond the current block-addressing range are unsupported.**
- NTFS write/repair, compressed/encrypted streams and Windows journal recovery
  are not supported.

## Using the system

1. Boot the new `kvantos.iso`; an older installed kernel does not update itself.
2. USB import selects a file with the mouse and copies it to RAM. The source is
   unchanged; persistence requires KvFS or an explicitly selected/writable FAT32
   destination.
3. FAT remains read-only by default. On a pre-provisioned owned-WAL FAT32 volume,
   inspect status/recovery first; then, if needed, run `fatwrite VOLUME enable`
   for this session. Use the actual volume name. Rescan requires a new enable.
4. After uncertain FAT I/O, recover the journal in KvantOS before external writes
   or `fsck` repair. Other operating systems' FAT drivers do not understand this
   WAL.

See [STORAGE-M2.md](docs/STORAGE-M2.md), [M3.2 AHCI](docs/STORAGE-M3.2.md),
[USB guide](USB-IMPORT.md), [C Workshop](sdk/WORKSHOP.md), and [installation](INSTALL.md).

## Validation and evidence limits

The local release gate completed with **GCC 12.4.0, QEMU 10.0.13 and OVMF
2025.02**:

- `make CC=gcc-12 release` and `tests/boot/check-images.py`: BIOS32, UEFI64,
  EFI, GRUB ISO, direct ISO, floppy, 11 apps per architecture, empty KvFS,
  firmware entries and SHA-256 manifests — PASS.
- QEMU BIOS32/UEFI64 boot from CD, direct ISO and firmware USB reached `kvsh`;
  BIOS floppy fallback — PASS.
- GUI/native app, C Workshop, mouse editors, EN/RU input/search/replace,
  reboot and KvFS persistence — PASS on both architectures.
- EHCI+OHCI USB Import on GPT and MBR — BIOS32 + UEFI64, with unchanged source
  image SHA-256. AHCI read-only/EIO, mixed AHCI+USB owned-WAL saves and GUI Save
  As to enrolled FAT32 — PASS; recovery and independent fsck were included.
- EHCI per-controller isolation/detach fencing — PASS on both architectures.
- MCC native compiler + ASan/UBSan parser/VFS/controller suites, GPT, FAT/WAL
  fault/performance, NTFS actual-image suite, and separate M6.20 ASan/UBSan run
  — PASS. NTFS fixtures include an exact 4-GiB `$DATA` and a poisoned fragmented
  Initialized-Size tail; all images remain unchanged.
- Test images were temporary regular files: **zero physical devices and host
  mounts**. QEMU/software results do not qualify physical USB/SATA media or
  VirtualBox.

## Honest limitations

- Experimental OS. Keep backups; do not store the only copy of important data.
  FAT writes assume one writer, truthful flushes and a correctly provisioned WAL.
  Torn/corrupt journals may intentionally block mounting; this is fail-closed, not
  automatic repair.
- AHCI hotplug, xHCI/USB 3, UAS, NVMe and ATAPI are unsupported. EHCI hub/TT
  routing remains limited and experimental; arbitrary hub trees, split bandwidth,
  multi-TT behavior and physical interoperability are not claimed.
- Physical hardware, VirtualBox passthrough, physical 4Kn and concurrent external
  writers are unqualified. One cause of an earlier parallel x64 Save As refusal
  remains open; stress reliability is not claimed.
- Native C Workshop is a limited language subset and not a security sandbox; do
  not run untrusted code. Full app isolation, GCC/ISO C, full Unicode case-folding
  and NTFS write/repair are outside this release.
- FAT filesystem repair, GPT repair and automatic WAL provisioning are absent.

## Release files

- `kvantos.iso` — primary BIOS/UEFI ISO and firmware USB-boot image.
- `kvantos-direct.iso` — GRUB-free BIOS/UEFI ISO fallback; not a raw USB hybrid.
- `kvantos-floppy.img`, `kvantos-disk.img` (empty KvFS), `kvant.bin`,
  `kvant64.bin`, `kvantefi.efi`, and `esp.img`.
- `kvantos-3.1.4.tar.gz` — full bundle with applications, docs and SHA256SUMS;
  `kvantos-3.1.4-source.tar.gz` — source snapshot;
  `kvantos-3.1.4-SHA256SUMS.txt` — checksums for individual assets.

BIOS/QEMU:

```sh
qemu-system-i386 -m 256 -boot d -cdrom kvantos.iso \
  -drive file=kvantos-disk.img,format=raw,if=ide
```

UEFI/QEMU (OVMF, Secure Boot disabled):

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

On q35, plain `-drive if=ide` normally binds to unsupported AHCI; the example
uses the explicit PIIX3/IDE controller. BIOS/QEMU persistence uses ATA PIO. In the
full bundle verify `sha256sum -c SHA256SUMS`; for standalone assets use
`sha256sum -c kvantos-3.1.4-SHA256SUMS.txt`.

License: MIT. Read `INSTALL.md` and back up data before installing.
