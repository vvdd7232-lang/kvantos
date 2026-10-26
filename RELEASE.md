# KvantOS 3.4.0 “Breeze” — a light desktop

October 26, 2026. Update for **BIOS/i386** and **UEFI/x86-64**.
Existing 3.3.0 tags, releases and downloads remain unchanged.

## Highlights

A Windows 7-inspired native desktop: softly tinted title gradients, white
fields, restrained borders, shadows and procedural Breeze wallpaper. This is
freestanding C rendering, not a web shell; glass is stylised, without blur or
actual transparency.

- Light windows, menus, file manager, editors and dialogs.
- Caption minimise button and taskbar restore.
- Adaptive task slots keep all eight windows reachable at 640×480 without
  covering the date and clock. Memory statistics remain in Monitor.
- Blue accent and Breeze wallpaper by default; seven accents and six wallpapers.
- Matrix remains an app and wallpaper; the terminal intentionally stays dark.

## Layout fixes

Symmetric rounded corners, bounded title gradients, aligned close-button lines,
centred and UTF-8-fitted button text, title/control separation and launcher
column gutters. About wraps long details and reports the build architecture
and bootloader correctly. Help has wrapping and Back/Next pages at its native
window size. Paint's label no longer overlaps the colour swatches.

## Compatibility

No application ABI, file-format or disk-write policy changes. Ordinary FAT
mounts remain read-only; optional writes still require a prepared owned-file
WAL and explicit per-session enablement. Existing USB/AHCI/NTFS restrictions
remain; physical devices and VirtualBox are not qualified by this release.

Saved accent/wallpaper indices remain compatible. Existing Matrix preferences
are preserved: choose **KVANT → Settings → Appearance → Breeze** to switch.
Neon is now called Green and accent shades were adjusted for readable text.
Previously downloaded ISOs do not update themselves.

## Verification and publication

Native host GUI raster/layout tests under ASan/UBSan, editor-model tests and
EN/RU input tests passed locally. Documentation previews use actual GUI code
with deterministic device stubs, not a booted VM.

The GitHub release is published only after CI builds and validates the images,
BIOS/UEFI CD/USB boot, desktop, editors, C Workshop, compiler and FAT/WAL,
NTFS and AHCI/USB scenarios. Host rendering does not replace boot testing.

## Artifacts

- `kvantos-3.4.0.tar.gz` — distribution: ISOs, kernels, EFI, apps and SDK;
- `kvantos-3.4.0-source.tar.gz` — tagged sources;
- `kvantos.iso`, `kvantos-direct.iso`, `kvantos-floppy.img`, `kvantos-disk.img`;
- `kvant.bin`, `kvant64.bin`, `kvantefi.efi`, `esp.img`;
- `kvantos-3.4.0-SHA256SUMS.txt` — SHA-256 checksums.
