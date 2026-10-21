# KvantOS 3.3.0 - dark hacker theme and Matrix

October 21, 2026. Release for **BIOS/i386** and **UEFI/x86-64**. Existing
3.2.0 tags and downloads remain unchanged.

## At a glance

3.3.0 is the design release. The interface is now dark: graphite panels,
black windows, a neon-green accent and green code on the desktop. A built-in
**Matrix** application brings an animated digital rain. Everything from 3.2.0
is preserved: both kernels, C Workshop, the editors, KvFS and the conservative
FAT32 write policy.

## Changes since 3.2.0

### Dark interface

- Black and graphite panels, windows, menus, taskbar and the KVANT launcher.
- Neon green is the default accent; text and borders are tuned for contrast
  on dark surfaces.
- Dark search, replace and import dialogs; editor selection no longer blends
  into the text.
- The kvsh prompt and status lines are green.

### Matrix

- New **Matrix** wallpaper with green code; it is the default desktop.
- Built-in **Matrix** application: animated digital rain in a 560x400 window,
  the `root@kvant:~# ./matrix` line and a blinking cursor.
- Matrix is the eleventh icon in the menu and on the desktop.

### Settings

- Seven accents: Neon, Blue, Teal, Emerald, Purple, Garnet, Graphite.
- Five wallpapers: Gradient, Night, Grid, Solid, Matrix.
- The accent and wallpaper choices are saved in `settings.cfg` and restored
  after a reboot.

## Compatibility

- BIOS/i386 (`kvant.bin`) and UEFI/x86-64 (`kvant64.bin`), hybrid ISO.
- All 3.2.0 applications and formats are preserved; FAT32/USB policy is
  unchanged.
- The application ABI is unchanged; existing `.kapp` files still run.

## Verification

CI builds both kernels and ran 16/16 jobs: GUI and editors on BIOS and UEFI,
EN/RU input, find/replace, RAM and KvFS persistence, C Workshop, compiler
isolation, AHCI/USB and the storage scenarios.

## Artifacts

- `kvantos-3.3.0.tar.gz` - distribution (ISOs, kernels, EFI, apps, SDK);
- `kvantos-3.3.0-source.tar.gz` - full tagged sources;
- individual images and kernels: `kvantos.iso`, `kvantos-direct.iso`,
  `kvantos-floppy.img`, `kvantos-disk.img`, `kvant.bin`, `kvant64.bin`,
  `kvantefi.efi`, `esp.img`;
- `kvantos-3.3.0-SHA256SUMS.txt` - SHA-256 checksums.
