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
