# M3.2 — AHCI SATA writes, barriers and owned-WAL recovery

M3.1's read-only transport restriction is superseded by this **software** stage.
This is not physical SATA qualification, completion of all M3, or an automatic
release. M2 physical USB/VirtualBox acceptance remains deferred by the user.

## Default policy and enabling a FAT32 volume

All ordinary FAT mounts still start RO, including SATA. Only a FAT32 volume
explicitly enrolled with the existing `tools/fat-wal.py` may opt in:

```text
mount
fatwrite sata0p2 enable
```

Use the actual volume name, not the example. Shared mouse Save As selects the
volume/folder; subsequent Save remains bound to that destination. Disable with
`fatwrite sata0p2 disable`. A rescan or reboot clears filesystem write opt-in.
Unprepared media, FAT16 and NTFS never acquire this FAT32 write permission.
The kernel does not create, reset or guess a journal during mounting.

Preparation uses the SAME M2 owned-file format and held-FD/staged provisioner.
It never uses spare sectors, gaps, MSR or GPT. Start on disposable **regular-file
images**, with backup. Initial allocation/provisioning is not crash-atomic.
No actual physical preparation or writes were performed for this stage.
See [STORAGE-M2.md](STORAGE-M2.md) for enrollment and cross-OS obligations.

Enrollment permits automatic recovery of a valid pending WAL BEFORE namespace
publication. New ordinary saves still require per-session enable. Invalid WAL,
ambiguous ownership or failed recovery withholds the namespace, not a guessed RO
view. If the disk lacks advertised FLUSH CACHE EXT support, it stays read-only;
a pending WAL requiring recovery cannot be exposed on that transport.

## Transport implementation

- IDENTIFY + READ DMA EXT remain available without a write lease.
- WRITE DMA EXT `0x35`, header W bit and private DMA OUT bounce buffer. u8-sized
  requests split into up to four 64-sector commands; no direct caller-buffer DMA.
- FLUSH CACHE EXT `0xEA`: no PRDT and zero byte count; success only after actual
  command completion with no ATA/AHCI error. Capability is checked in IDENTIFY.
  This flushes the device cache, not merely a selected partition's bytes.
- Write and flush entry points independently require a short-lived FAT operation
  lease: current media generation, exact nonempty filesystem interval within
  capacity. Caller-supplied out-of-range/overflow/null/zero requests are refused.
  A clean enrollment check grants and immediately revokes a probe lease before
  advertising RW, so unsupported flush capability is not mistaken for writeability.
- One common block-generation/lease router for USB and AHCI; ATA keeps its existing
  bounded path. Stable disk IDs unchanged: ATA 0..3, USB 4..11, SATA 12..19.
  Generic FAT and editor destination guards now validate SATA generations too.
- AHCI namespace rescan revokes grants and advances surviving-port generations.
  It does NOT recycle pinned buffers, un-fence a failed port, discover newly
  inserted SATA devices or implement hotplug. Port faults require a reboot.
- Link/PHY/error latches are NEVER cleared between a lease check and issuing a
  command. Only normal completion bits are acknowledged; status is rechecked
  immediately before issue. No silent write retry after a disconnect/error.
- Write/flush errors, short DMA, timeout and link change fence the port, revoke
  the lease and keep its private DMA structures pinned, even if stop fails.
  The controller/port is not reset or reinitialized behind a live filesystem.

The FAT/STX algorithm itself remains the shared M2 protocol: COW bodies + actual
sync, backup + sync, indexed undo header + sync, metadata update, commit barriers,
then release. Recovery validates every backup/target/owner before obtaining a
transport lease. Both FAT copies, directory entries and BPB-root disjointness
remain mandatory. No bespoke SATA filesystem or second WAL format was added.

## Uncertain failure and late completion

A timeout is NOT proof that a device did nothing. The UI must keep text, Undo and
the unsaved checkpoint, and must NOT print a successful save. The late command
may nevertheless finish. In particular a journal-close write/flush may make the
new version durable after the timeout was reported. After cold restart either a
complete recovered-old view or a complete committed-new view is acceptable;
partial/mixed namespaces and a false success checkpoint are not.

The GUI test checks the valid WAL header to distinguish these outcomes, compares
the COMPLETE ordinary namespace and data to the corresponding expected state,
and runs independent fsck. It does not assume every unsuccessful save must lose
the new version. Pinned per-port buffers keep late DMA away from document memory.

After uncertain I/O/power loss, recover in KvantOS BEFORE external writes or fsck
repair. Other OS FAT drivers do not understand this WAL; dirty bit is only a hint.
Exclusive single writer/media, honest flush and the M2 sector-tear model remain
assumptions. No protection from malicious devices, arbitrary bit rot or concurrent
raw writes. A very fast physical replacement during an in-flight command is not
claimed safe by software-only tests; disconnect/reconnect is not a supported SATA
workflow in this driver. Do not share a live writable image/device with another OS.

## Reproducible disposable-image checks

```sh
make -j4 CC=gcc-12 STORAGE_LAB=0 iso
make -C tests/mcc CC=gcc-12 ahci storage vfs editors input ramfs
STORAGE_CONTROLLER=ahci STORAGE_LAYOUT=gpt python3 tests/boot/fat-owned-smoke.py
STORAGE_CONTROLLER=ahci STORAGE_LAYOUT=mbr python3 tests/boot/fat-owned-smoke.py
GUI_SAVE_OWNED=1 STORAGE_CONTROLLER=ahci STORAGE_LAYOUT=gpt python3 tests/boot/save-as-smoke.py
GUI_SAVE_OWNED=1 STORAGE_CONTROLLER=ahci STORAGE_LAYOUT=mbr python3 tests/boot/save-as-smoke.py
python3 tests/boot/ahci-usb-smoke.py
python3 tests/boot/ahci-smoke.py
```

The shared FAT and GUI scripts still support their previous USB modes. No normal
kernel test hooks or kernel-memory edits are used. Debugger stops are function
breakpoints in ordinary STX/VFS code. QEMU ICH9 AHCI's IDE bus rejects actual disk
hot-unplug: do NOT relabel the SATA tests as real unplug qualification. Power kills,
blkdebug EIO and real suspended flush timeout/late completion are separate checks;
link loss/replacement latches/stuck engines are also exercised in the host model.

QEMU IDE refuses a truly read-only AHCI block node at construction. The SATA
write-denied case therefore uses permanent blkdebug EACCES and proves the whole
image unchanged; USB separately exercises an actual read-only SCSI backend.
Neither is physical hardware write-protect qualification.

BIOS/i386 and UEFI/x86-64, MBR/GPT, empty/multicluster/full/write-denied/faults, cold recovery
before mount, namespace/content/whole outside-partition guards and fsck are checked.
Mixed-bus tests exercise separately enabled SATA and USB WAL volumes in the SAME
VM, both USB controller families, cold and warm reset, and rescan back to RO.
CI workflow definitions are not evidence that a remote job has run.

## Still not supported / open acceptance

Physical SATA and USB, VirtualBox, 4Kn, >u32 sector-count capacity, ATAPI/CD driver,
NCQ, port multipliers, sleeping-link management, IOMMU and SATA hotplug remain
unqualified/unsupported as documented in M3.1. Installer, KvFS, format and settings
remain legacy ATA-only. OHCI is implemented separately in [M3.3](STORAGE-M3.3.md),
not UHCI renamed; hubs/routing, xHCI and NVMe remain independent future stages.
