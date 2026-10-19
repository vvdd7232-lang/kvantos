# KvantOS 3.2.0 — smooth graphics and a friendlier desktop

October 19, 2026. Release for **BIOS/i386** and **UEFI/x86-64**. Existing
3.1.x tags and downloads remain unchanged.

## At a glance

3.2.0 is the visual release. The hard bitmap font is replaced by an
anti-aliased one, the window interface is redrawn with smooth shapes, and
the desktop explains itself: icon tooltips, program descriptions in the
menu and a first-run hint line. The 3.1.4 storage policy is unchanged:
ordinary FAT volumes stay read-only, writes go only to a prepared FAT32
volume with an owned WAL after an explicit enable, and NTFS is strictly
read-only.

## Changes since 3.1.4

### Graphics without rough pixels

- **Anti-aliased text everywhere**: a coverage font (DejaVu Sans Mono,
  full Cyrillic) in the same 8x16 CP866 cell — smooth letters in the GUI,
  terminal, editors and the boot console. The legacy bitmap font remains
  as the fallback used when smoothing is off.
- **New primitives**: anti-aliased (Xiaolin Wu) lines, supersampled discs,
  rounded rectangles and title gradients, soft layered window shadows.
- Windows, the round close button, desktop icons, the KVANT launcher and
  the taskbar were redrawn with these shapes; bold headings and a big
  smooth K V A N T logo on the desktop.

### Clear and convenient

- Hover the pointer over an icon: a highlight and a tooltip explain what
  the program does; the same one-line descriptions appear next to the
  names in the KVANT menu.
- A first-run hint line at the bottom of the desktop.
- The date sits next to the taskbar clock; the right-hand readouts can no
  longer overlap at small widths.
- Settings > Appearance gains a "Smooth (anti-aliased) text" toggle.
- The kvsh status line no longer paints a blue strip over the desktop
  every half second.

### Internals

- Font generator `tools/mkfont_aa.py` (3x rendering + downsampling, packed
  at 4 bits per pixel — the font costs 16 KiB in the kernel image).
- Direct-ISO builder: the bounce ceiling is raised to 0x8E000 (the
  Multiboot info block stays at 0x90000).

## Verification

Locally and in CI: both kernels built; gui-smoke (GUI, 11 applications,
Maze 3D) on BIOS and UEFI; mcc smoke (C Workshop); file manager; editors;
the release workflow storage suites. Known limitation: the patched-QEMU
storage LAB suites are not part of this release gate and keep running in
the main CI pipeline.

## Artifacts

- `kvantos-3.2.0.tar.gz` — distribution (ISOs, kernels, EFI, apps, SDK);
- `kvantos-3.2.0-source.tar.gz` — full tagged sources;
- individual images and kernels: `kvantos.iso`, `kvantos-direct.iso`,
  `kvantos-floppy.img`, `kvantos-disk.img`, `kvant.bin`, `kvant64.bin`,
  `kvantefi.efi`, `esp.img`;
- `kvantos-3.2.0-SHA256SUMS.txt` — SHA-256 checksums.
