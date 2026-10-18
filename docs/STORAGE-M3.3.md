# M3.3 — OHCI full-speed root-port storage

This software stage adds an actual OHCI 1.0 backend (PCI class 0c/03/10).
It is NOT UHCI renamed. It shares the existing USB enumeration, BOT/SCSI, block
IDs, FAT/VFS, owned WAL and mouse editors. No new filesystem or raw write command.
M2 physical USB/VirtualBox and physical SATA acceptance remain open/deferred.

## Contract

- Direct full-speed (12 Mb/s) root-port USB mass storage, BOT/SCSI transparent
  command set, 512-byte sectors, READ/WRITE(10), SYNCHRONIZE CACHE(10).
- Separate HCCA, ED, real/dummy general TD and aligned 4 KiB bounce buffer per
  attempted OHCI controller. Up to four USB controllers total (UHCI/OHCI/EHCI),
  up to eight enumerated storage devices. OHCI root descriptor may have 1..15 ports.
- Static coherent x86 DMA below 4 GiB, mapped 32-bit MMIO BAR; no high-address DMA,
  periodic/isochronous/interrupt transfers, USB HID, low-speed storage, hubs or UAS.
- All filesystem mounts default RO. Explicitly enrolled FAT32 alone may enable
  session writes with `fatwrite VOLUME enable`. See M2/M3.2 for enrollment risks,
  owner validation, filesystem bounds, per-operation generations and WAL recovery.
  Enrollment permits recovery before publication, including on a default-RO mount.
  NTFS remains RO; no USB installer/KvFS target or automatic provisioning.

## Initialization and transfer lifetime

Revision, BAR, root-hub geometry, address/physical alignment and contiguous DMA
ranges are checked. Request firmware ownership (IR/OCR), refuse a stuck handoff
without taking its live schedules. Once firmware relinquishes, disable bus master
before reset and replacing its DMA lists. Disable all controller interrupts,
program HCCA/frame interval/periodic threshold with empty lists, enable DMA,
enter operational state and power the root ports. Verify advancing frame counter.

Control and bulk lists are enabled one at a time. A single general TD may cover
up to 4096 bytes, packetized by the controller at full-speed endpoint MPS.
Explicit DATA0/DATA1 seed and ED carry preserve endpoint toggles, including short
IN and zero-length status stages. Completion requires **ED retirement**, not
merely TD CC=NoError: a controller may write NoError after an intermediate OUT
packet while the rest of that TD is still pending. A dedicated host regression
models this case and fails with the original incorrect CC-only completion test.

Successful transfer detaches/skips the list and waits two frames before descriptor
reuse. OUT data is copied before DMA; IN data reaches the caller only after valid
completion and retirement. Per-controller busy guard prevents reentrant reuse.
Link/change/UE evidence is checked again before issue, never acknowledged away at
that boundary. Root change bits are cleared only after successful enumeration.

Stall, protocol/descriptor error, short OUT, timeout, link loss during a transfer,
UE, stopped clock or failed retirement fences that controller until reboot,
disables lists and bus mastering, and retains ALL of its private DMA memory.
No software BOT/WRITE replay, reset/reinit or pinned-pool recycling after faults.
This is deliberately conservative: BOT recovery cannot revive a fenced controller.
Other controllers' DMA memory is disjoint and remains usable.

Idle unplug followed by rescan/replug is supported: fresh namespace and explicit
new write opt-in. Unplug/error during active I/O may require a reboot of the
controller; do not promise a rescan will revive it. Generations invalidate cached
sectors and saved editor destinations. Arbitrarily fast physical replacement,
malicious hardware and IOMMU isolation are not qualified by software models.

## Disposable-image reproduction

```sh
make -j4 CC=gcc-12 STORAGE_LAB=0 iso
make -C tests/mcc CC=gcc-12 ohci ahci storage vfs editors input ramfs check
USB_CONTROLLER=ohci USB_LAYOUT=mbr python3 tests/boot/usb-import-smoke.py
STORAGE_CONTROLLER=ohci STORAGE_LAYOUT=mbr python3 tests/boot/fat-owned-smoke.py
STORAGE_CONTROLLER=ohci STORAGE_LAYOUT=gpt python3 tests/boot/fat-owned-smoke.py
GUI_SAVE_OWNED=1 STORAGE_CONTROLLER=ohci STORAGE_LAYOUT=gpt python3 tests/boot/save-as-smoke.py
python3 tests/boot/ahci-usb-smoke.py
```

Both kernels run by default; `MCC_ARCH64=0|1` selects BIOS/i386 or UEFI/x86-64.
Standalone `pci-ohci` + `usb-storage` is actually negotiated at 12 Mb/s, checked
through QEMU `info usb`, with the native OHCI counter asserted. Tests use ONLY
private regular-file images, never physical devices, mounted partitions or a host
filesystem import workaround. Actual normal-kernel STX debugger cuts, device_del,
blkdebug EIO/read-only backend, fsck and outside-region hashes are independent.
No normal-kernel pause hooks or guest-memory edits are needed.

Host test uses the real private backend with simulated MMIO/DMA: firmware/reset/
frame failures, invalid addresses, partial OUT/short IN, setup/status/toggles,
error and descriptor corruptions, pre-issue change latch, failed retirement,
private pools and late DMA isolation. Register models are NOT physical proof.
The existing EHCI + OHCI-companion setup still runs HS storage on EHCI. Do not
mislabel that historical HS test as an OHCI mass-storage test. Existing UHCI/EHCI
write/coexistence regressions are separate from standalone OHCI qualification.

## Local software acceptance

Final normal kernels: both MBR/GPT console parents (four firmware/layout cells),
standalone OHCI 12 Mb/s read/import/native apps, GUI Save As/unplug (both kernels)
all passed. Six mixed SATA/USB cells (three USB families x both kernels), AHCI
read/default-RO and EHCI GUI regressions also passed. Real outside-image hashes
and independent fsck required, not just UI success. Fresh feature-source builds
reproduce frozen kernels/apps with recorded banner times; boot/image checks pass.
Remote CI and hardware acceptance were not run. Package VALIDATION.txt includes
full commands, the initial completion bug/regression and exact artifact hashes.

## Remaining work

Physical OHCI/VirtualBox USB 1.1 and EHCI companion full-speed handoff still need
actual device acceptance. No hubs, split transactions, xHCI, NVMe or completion
of the entire M3 is implied. USB hubs and port routing are the next separate step;
keep the same bounded I/O/leases/recovery and regression obligations.
