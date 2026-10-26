# Native GUI visual checks

```sh
make -C tests/gui check
```

Requires GCC with ASan/UBSan, make and Python 3; no third-party Python packages,
NASM, QEMU or firmware required. Outputs are ignored under `build/gui-visual/`.
`CC=gcc-12` may be supplied to match the release compiler.

The host executable includes the **real** `kernel/gui.c` and `kernel/fb.c`,
uses the kernel fonts, charset, text and formatting routines, and supplies
fixed time/CPU/video information instead of accessing privileged devices.
Unused kernel entry paths are removed by linker garbage collection.

Checks:

- default Blue accent and appended Breeze wallpaper index 5;
- EN/RU desktop, Help/About window, Appearance and launcher PNG snapshots;
- all 7 accents × 6 wallpapers at 640×480 and 1024×768 under ASan/UBSan;
- desktop icon bounds, icon hit regions, smooth/bitmap widget and frame paths;
- all eight taskbar slots at both resolutions, seconds on/off, matching input
  geometry, no clock-region hits and no clickable gaps;
- caption close/minimise boundaries;
- all Appearance controls inside the Settings client at both resolutions/languages;
- pixel symmetry of all four rounded corners and both sides of title gradients,
  including odd/even dimensions and AA on/off; no pixels outside gradient bounds;
- diagonal line endpoints (the caption X no longer shifts half a pixel);
- strict UTF-8 truncation for 0–3-cell labels, word/hard wrapping, narrow buttons
  and long window titles that must not touch caption controls;
- native 600×440 Help pagination, Back/Next hit regions and all pages staying
  inside the client; native 420×300 About wrapping without desktop spill.

The small-window PNGs use these native sizes rather than enlarged demo windows.

Snapshots are **host-rendered native GUI output, not QEMU screenshots**.
The assertions exercise rendering and layout contracts, not a golden-image
pixel comparison or a complete GUI event loop. In particular they do not prove
boot, PS/2 input, minimisation/restoration of running apps or file persistence.

For those checks, on a fully provisioned system also run:

```sh
make release
bash tests/boot/qemu-smoke.sh release/kvantos.iso
python3 tests/boot/gui-smoke.py
python3 tests/boot/editor-smoke.py
```

See [boot test requirements](../boot/README.md). The common mouse driver uses
the same adaptive task-slot formula as the kernel. Existing release images
must not be substituted for rebuilt images when testing the new interface.
