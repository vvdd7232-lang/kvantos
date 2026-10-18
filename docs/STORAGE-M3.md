# M3.1 — AHCI SATA, first read-only stage

Historical M3.1 contract below. **M3.2 supersedes the transport RO restriction:**
[AHCI write leases, FLUSH EXT and recovery](STORAGE-M3.2.md). All mounts still default RO.

M2 software implementation remains available. The user deferred physical USB and
VirtualBox passthrough acceptance; those boxes are still OPEN, not passed by QEMU.
M3.1 is NOT completion of the entire AHCI driver or of M3.

## Implemented contract

- PCI class/subclass/prog-if `01/06/01`, type-0 header, 32-bit non-prefetchable
  ABAR below 4 GiB and outside low identity RAM; uncached MMIO mapping.
- BIOS/OS ownership handoff when advertised. After handoff, PCI bus mastering is
  disabled and **all implemented firmware engines**, including ATAPI, are stopped.
  DMA is re-enabled only if every engine quiesces. Unsupported CD devices are
  not exposed; all applications have already been loaded by the bootloader.
- Active directly attached ATA SATA ports, IDENTIFY, LBA48, **512-byte logical
  sectors**, nonzero capacity fitting the existing u32 sector-count ABI.
  4Kn, ATAPI, port multipliers, sleeping/spin-up links and >u32 capacity are not
  supported. 512e is not ruled out solely by its physical sector size, but has
  not been physically qualified.
- Up to eight attempted SATA ports across the enumerated PCI controllers. A
  rejected/faulted attempt consumes its private buffer slot; it is not recycled.
- One polled command slot, one PRDT, coherent x86 DMA and pinned static buffers.
  CLB/FB/table alignment, physical contiguity and <4 GiB address limits checked.
  Reads use a 32 KiB bounce buffer; u8 requests split into at most four commands.
  User/driver buffers are never used directly as DMA targets.
- Only IDENTIFY `0xEC` and READ DMA EXT `0x25` are accepted by command construction.
  No SATA WRITE/FLUSH/TRIM command path exists. Block writes and sync fail for
  AHCI independently of filesystem/UI flags. No IOMMU support is claimed.
- Busy guard; finite tick AND iteration budgets for handoff, engine stop and I/O.
  Short DMA, task/bus/interface error, timeout or link/PHY-change latch fences
  the port until reboot. The command cannot be retried on a newly inserted disk.
  Old namespace reads fail after fencing; rescan removes inaccessible volumes.
- A failed stop leaves private DMA memory pinned and unavailable for reuse.
  Other ports may continue with their own buffers. No automatic COMRESET,
  controller reset, hotplug/reconnect or live reinitialization in this stage.

## Filesystem/UI integration

Stable block IDs are ATA `0..3`, USB `4..11`, AHCI `12..19`; absent slots return
zero capacity. Existing USB indices, generations and M2 WAL leases stay unchanged.

The ordinary partition scanner reads raw FAT, MBR and GPT on SATA too. Names:
`/mnt/sata0`, `/mnt/sata0p1`, `/mnt/sata0p2`, etc. GPT slot numbers are preserved.
FAT uses the common VFS, file manager and mouse import in both editors. NTFS
remains the existing read-only driver; M3.1 guest qualification covers FAT32,
not a new SATA/NTFS matrix. FAT free-space display retains the existing bounded
sampling implementation; it is not an exact free-space promise.

`disk` lists ATA and SATA separately and shows SATA `DMA, read-only` or
`fenced; reboot required`. `mount` shows SATA volumes as RO. Even a clean,
preprovisioned `KVANTWAL.SYS` cannot enable SATA writes via `fatwrite`.
A pending WAL requires writes for recovery: **withhold the FAT namespace**, do
not discard the log, pretend recovery succeeded or mount an uncertain view.
Do not use another OS to edit such a volume before qualified recovery.

KvFS, `format`, installation and settings remain **legacy ATA-only**. An AHCI
controller is not an installer destination and is not silently treated as ATA.
SATA -> RAM/KvFS copying is supported when the separate target exists; SATA
source stays unchanged. QEMU SATA backend is deliberately writable in the tests
so RO is a tested OS policy, not merely a backend permission.

## Reproducible checks (disposable images only)

```sh
make -j4 CC=gcc-12 STORAGE_LAB=0 iso
make -C tests/mcc CC=gcc-12 ahci
python3 tests/boot/ahci-smoke.py
python3 tests/boot/ahci-usb-smoke.py
```

Dependencies: GCC/multilib, GRUB, NASM, QEMU x86, OVMF 4M, Python/Pillow,
dosfstools, mtools. The scripts create only regular files under `build/`.
They never prepare a real block device.

- ASan/UBSan runs the real driver against a register/DMA model: two controllers,
  port limits, firmware/ATAPI engine retirement before DMA, FIS/PRDT layouts,
  unaligned 255-sector buffers, bounds, short reads, errors and quarantining.
  Hardware-register simulation is not a physical SATA result.
- Real QEMU ICH9 AHCI on BIOS/i386 and UEFI/x86-64, two SATA ports + IDE/KvFS,
  raw/MBR/GPT FAT32, byte-exact multi-cluster VFS copies, RO refusal, rescan,
  full power-cycle reads, LFN import in both editors, actual blkdebug read EIO,
  pending-WAL refusal, whole-source hashes and independent fsck.
- Mixed AHCI RO + normal owned-WAL USB RW, both kernels, UHCI/MBR and
  EHCI+OHCI/GPT: USB create/overwrite/cold-read/fsck while SATA remains RO and
  byte-identical. GPT/gaps and USB journal owner remain unchanged.
  This is a coexistence regression, NOT a rerun of all M2 crash cuts.

CI job `storage-ahci` runs the two guest scripts and uploads logs/screenshots,
not private raw disk images. Workflow definition is not a remote CI result.

## Next stages, still open

M3.2: separately gated AHCI WRITE DMA EXT/FLUSH, filesystem ownership/write lease,
error handling and power-loss/recovery tests on disposable images before any
physical write qualification. AHCI installation is a separate decision.
OHCI is now a separate [M3.3 software stage](STORAGE-M3.3.md). Hubs/split
transactions, xHCI and NVMe remain subsequent roadmap tasks.
No physical SATA, VirtualBox SATA/USB, 4Kn or IOMMU acceptance is claimed here.
