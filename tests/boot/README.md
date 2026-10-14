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
