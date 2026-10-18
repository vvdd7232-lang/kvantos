# M2: storage write safety — status and reproduction

## Status

M2.1 through M2.4 software paths are implemented locally. **Whole M2 is not
hardware-qualified.** Normal kernels now support an explicitly owned FAT journal
file, recovery before namespace publication, and per-session FAT32 opt-in. All
FAT mounts default RO. No arbitrary gap/reserved sectors or partition-table writes
are used; no raw USB write grant is exposed to the shell. The `usbtest`/`fattest`
commands remain lab-only. No physical removable device or VirtualBox passthrough
has been write-tested, and no push/release/remote Actions run is implied.

See [M2.4](#m24--normal-owned-file-wal-supersedes-the-earlier-normal-ro-only-policy)
for the current protocol/constraints. Earlier M2.1–M2.3 sections record their
historical policies and tests; the later normal-owned policy supersedes them.

### KvFS

Replacement allocates a **new** contiguous extent while the old one remains live.
Write/flush every new body sector first; then publish only the affected directory
sector. No separate superblock count write can fail after publication. The v2
`file_count` field is advisory: directory flags are authoritative on mount, in
old kernels and in `sdk/mkdisk.py`. A safe replacement needs spare contiguous
space even when shrinking; no-space never frees the old extent. Empty files use
one zeroed sector, matching the existing format.

A body write failure leaves the old directory and body available. A directory
write failure is **ambiguous** (the device may have committed before reporting an
error); the filesystem is fenced/unmounted until an explicit remount. We never
retry that save or flush stale in-memory directory entries afterward. Remount
validates geometry, names, extents, duplicate names and overlaps, then recounts
entries. GUI storage status preserves a write-error indication even when fenced.

This is **not** a journal, atomic-sector guarantee, or power-loss-proof filesystem.
A torn directory sector may still require recovery. Read/write barriers trust the
device's cache-flush acknowledgement, not infallible physical hardware.

### USB

The pure `usb_bot_command` module builds CBWs, checks direction/length, tag,
signature, CSW status/residue and handles transport/phase errors with BOT recovery.
A valid SCSI failure is distinct from a transport failure. Neither retries WRITE.
WRITE(10) uses <=8 sectors/4096 bytes per transaction. SYNCHRONIZE CACHE(10)
has IMMED=0 and must succeed before a caller claims persistence. The block API
exposes `blk_sync`; ATA writes also flush and propagate completion/flush errors.

USB errors invalidate cached reads and revoke the scratch grant. Disconnect or
port-change flags invalidate the enumeration generation, including a new device
reusing the same port; rescan discards all grants. REQUEST SENSE records fixed or
descriptor-format sense data when available. Unsupported cache synchronization
is an error, not silently accepted success. Transfer/recovery waits remain
bounded; these are cycle/frame bounds, not a calibrated real-hardware SLA.

## Run tests (private regular files only)

```
make -C tests/mcc CC=gcc-12 storage
make -C tests/fs CC=gcc-12 gpt
make CC=gcc-12 STORAGE_LAB=1 iso
python3 tests/boot/usb-write-smoke.py
USB_CONTROLLER=uhci USB_LAYOUT=mbr python3 tests/boot/usb-write-smoke.py
# Crucial: restore normal configuration; dependency stamp rebuilds both kernels.
make CC=gcc-12 STORAGE_LAB=0 release
```

Tools: gcc-12/multilib, nasm, GRUB, xorriso, QEMU, OVMF, dosfstools, mtools, Python.
The lab script runs both kernels unless MCC_ARCH64=0/1 selects one. It constructs
GPT/MBR + FAT32 images, puts a capacity/version-bound marker **after the FAT
partition**, and writes only a 32-sector scratch interval after that marker.
The byte-for-byte comparison covers everything outside the interval, including
MBR/GPT, FAT copies, directories and FSInfo. `fsck.fat -n` is run on an extracted
FAT partition; no host mounts, block devices or formatting of real media occur.

Cases: default/range lock, 17-sector multi-CBW write, read-cache invalidation,
SYNCHRONIZE CACHE, cold guest readback, missing marker, read-only device, blkdebug
write EIO and flush EIO, disconnect during an armed bounded write-loop, return
within the test deadline, and subsequent reconnect/cold readback. BOT host tests
also cover short packets, malformed CSWs and phase/status/residue errors. KvFS
host tests cover faults before/after each body/metadata write, full media,
shrinking/growing/empty saves, corrupt mounts and 1500 reference-model operations.

The scratch marker and lab compile switch prevent accidental use; they are not
security isolation against privileged native apps. Lab ISO files must not be
published as normal releases. The PR storage job uploads **logs only**.

## M2.2a: FAT copy-on-write and explicit host-fixture recovery

The common FAT/VFS driver now prepares a separate new chain/body, synchronizes
it, publishes the directory entry and only then releases the old chain. Existing
8.3 aliases/LFN slots, creation times and attributes survive an overwrite. Full
media cannot destroy the original file to make room: COW needs spare clusters.
Empty saves, mkdir and remove use the same bounded metadata-undo mechanism.

Every metadata sector's original image is retained once. All mirrored FAT copies,
primary/backup FSInfo and directory/LFN sectors participate; a failed write/read/
barrier aborts, and a failed abort fences the volume (including subsequent reads).
The undo budget is **48 physical 512-byte sectors**, with 24 KiB in BSS, not the
8 KiB kernel task stack. Budget exhaustion fails rather than writing unlogged
metadata. Very fragmented/large operations may therefore be refused safely.
FAT scans read a table sector once per allocation scan, not once per entry.

Also corrected: read errors masquerading as EOC or successful partial file reads;
short-alias collisions/internal spaces; LFN checksum/ordinal validation and slots
across non-contiguous clusters; cross-cluster LFN deletion; file/directory/RO and
parent/name validation; geometry bounds and 4096-byte FAT sectors over a 512-byte
block API. Cycles are rejected before a chain is freed. Unsupported/overlong LFNs
fall back to their short alias rather than a potentially ambiguous truncated name.
New names are valid BMP UTF-8, with ASCII/Russian case-insensitive lookup; full
Unicode case folding and OEM short-name codepages are not implemented.

### What is actually compiled/enabled

* **Normal kernels:** bounded **RAM undo**
  plus FAT COW for the legacy ATA write path. This handles reported I/O failures
  when rollback succeeds; it is **not power-loss protection**. An unresolved
  failure requires stopping use of the volume and external checking, not treating
  rescan/reboot as a repair. There is no automatic persistent FAT recovery there.
* **Host test builds** (`FAT_TX_TEST` / `MCC_HOSTTEST`) and **explicitly enrolled
  STORAGE_LAB=1 images** (see M2.2b below): experimental durable
  sector undo journal. `fat_test_journal` is absent from both normal kernels.
  A test explicitly reserves 49 sectors **outside its FAT partition** in its own
  regular file. No gap/reserved sector on a user's disk is inferred to be free.
  Lab grants cover two separate owned intervals; MBR/GPT/NTFS remain outside them.
* Normal/unmarked USB FAT/VFS remains **RO**. There is still no editor
  volume/folder SaveAs. Lab images must not be installed or published.

The durable protocol writes backup+barrier, indexed CRC-protected header+barrier,
then target metadata. Commit synchronizes all data/metadata before clearing the
header and synchronizing again. All recovery addresses/backup CRCs are validated
before any undo write; replay can itself be interrupted and repeated. A failed
commit-close may mean old **or** new complete state: no further RAM rollback is
attempted after that irreversible boundary, and no success is returned on error.
A torn target can be restored from a valid log. A **torn/invalid journal header or
backup fails closed**; this prototype does NOT promise recovery from every possible
sector tear, arbitrary media corruption, dishonest flushes or lost journal media.

### Reproduce without mounting or touching any host device

```
make -C tests/mcc CC=gcc-12 storage
make -C tests/fs CC=gcc-12 fat-write
# Identical real-driver/VFS fault matrix under ASan + UBSan:
make -C tests/fs CC=gcc-12 build/fat-fault-sanitize
FAT_HARNESS="$PWD/tests/fs/build/fat-fault-sanitize" python3 tests/fs/test_fat_write.py
```

Tools: GCC, Python, dosfstools and mtools. The matrix creates sparse regular files,
formats only those files and launches fresh harness processes to recover before
file access. It checks external mcopy names/contents against **the complete old or
new namespace**, and runs `fsck.fat -n` after every recovered interruption.

Local results: **1048** before/after-write/barrier, EIO and unplug-style failures;
**40** torn-sector cuts (recover valid targets, refuse invalid headers); **55**
read-error injections; **99** handled write/barrier failures in the normal-kernel
RAM-undo path. Plus 65535-byte files, empty/full/RO media, alias punctuation and
collisions, real fragmented LFN boundaries/deletion, Russian case lookup, cyclic
old chains, and FAT16 with 512/4096-byte sectors. The standalone ASan/UBSan WAL
model also discards volatile write cache under three early-persistence policies:
**96** crash cuts and **30** interrupted recovery cuts, including ambiguous close,
budget exhaustion, invalid bounds and backup CRC refusal. These are **host**
restarts/cache-loss models, not FAT-write tests in a rebooted USB guest.

## M2.2b: actual USB FAT files and recovery BEFORE namespace publication

`STORAGE_LAB=1` now supports an additional **version-2 owned-image manifest**,
not a flag that enables arbitrary USB volumes. `tests/boot/mkfatlab.py` constructs
its own regular-file disk/MBR or GPT/FAT partition, reserves a post-partition
49-sector WAL and preformats it offline. A CRC-protected manifest binds capacity,
partition geometry, BPB CRC/serial and the exact journal interval. The guest never
formats a missing log or steals an apparently unused gap.

During FAT mount, the manifest is validated, **two disjoint USB write extents**
are granted (FAT partition and WAL only), recovery completes, and only then is
the volume registered in VFS as lab-RW. Any failed replay/invalid recognized
manifest/header/backup withholds the entire filesystem namespace and revokes the
grant. A normal kernel also refuses marked experimental volumes: it must not
expose interrupted test metadata without recovery. Unmarked devices stay RO.
The earlier v1 scratch marker/protocol still grants only its 32 raw sectors.

Mount I/O also checks a USB enumeration generation; neither an old mount nor an
old transaction can inherit grants belonging to a new enumeration/device. USB
errors/unplug still invalidate read caches and revoke all writes. Reconnect plus
rescan creates a new mount and replays the WAL before exposing files.

Lab ISO builds are deliberately **non-installable**: installer entry points are
disabled, no hdboot.img module is included, and the diagnostic menu boots without
the normal 10-second delay. Normal installation/media/menu behavior is unchanged;
`STORAGE_LAB=1 release` remains forbidden. Lab artifacts are **logs only**, not
normal releases or physical USB images.

```
make CC=gcc-12 STORAGE_LAB=1 iso
python3 tests/boot/fat-write-smoke.py
USB_CONTROLLER=uhci USB_LAYOUT=mbr python3 tests/boot/fat-write-smoke.py
# Keep the earlier transport protocol covered too:
python3 tests/boot/usb-write-smoke.py
USB_CONTROLLER=uhci USB_LAYOUT=mbr python3 tests/boot/usb-write-smoke.py
# Restore the safe normal configuration before ordinary use:
make CC=gcc-12 STORAGE_LAB=0 release
```

The FAT guest matrix runs BIOS i386 and UEFI x86-64, EHCI+OHCI/GPT and UHCI/MBR.
It uses actual common-VFS create/overwrite/empty/mkdir/remove/rmdir/65535-byte
operations, then cold VMs and external namespace/content comparison + fsck.
Eight trace-selected crash cuts include each FAT copy, primary/backup FSInfo,
directory publication and commit close. Backend write/flush EIO, actual USB
unplug/replug, stale generations even after a fresh validated RW grant, replay read/write failures, full/RO media and
corrupt manifest/log/backup refusal are also tested. Everything outside **both**
owned intervals, including MBR/GPT/owner/gaps, is compared byte-for-byte by hash.
The script kills QEMU at selected cuts, not just restarting a host FAT harness.
It does **not** emulate a host power failure or prove real-drive flush honesty;
the volatile-cache-loss/torn-sector model and fail-closed limits above still apply.

### M2.3a — editor VFS destination selection

Both editor instances use **Save as -> Destination...** (the Russian label is
`Том / папка...`). The existing quick local naming dialog still means RAM+KvFS,
with explicitly labelled *listing* filters, not a misleading RAM-only selector.
Destination browsing reuses the importer's directory cache, paging, Up and USB
refresh. It lists mounted volumes including RO FAT/NTFS, and reports RAM temporary
versus RW/RO. An explicit destination writes only the selected file via VFS;
there is no automatic fallback or second-volume write. Later Save, dirty-action
Save and workshop source Save retain that binding. Build output `.kapp` remains
local RAM/KvFS; long external names do not change the short local build alias.

Every existing destination file requires a separate replacement confirmation
in Save As (even when it is the current file); Cancel preserves the document,
Undo and old binding. Direct Save uses the binding without another name prompt.
RO, missing folder, directory collision, invalid/long names, full-volume and I/O
errors retain the buffer and do not advance the saved checkpoint or perform a
pending close/new action. Rescan invalidates namespace epochs; a path with the
same mounted name cannot silently inherit the old destination. USB generation
checks add transport identity fencing. This is not continuous hardware polling
or conflict detection against arbitrary external writers. A GUI FAT write
conservatively invalidates the other editor target on that volume, accounting
for case/SFN aliases without claiming a complete external-writer monitor.

Native names use physical ASCII: RAM max 23 bytes, KvFS max 39. FAT fields support
BMP UTF-8 up to 127 bytes, with path bounds and reserved-character validation;
workshop source names require `.c`/`.C`. The folder cache shows the first 128
entries (not a source-line cap). The editor still accepts up to 65535 text bytes,
120 bytes/line; exceeding 128 lines is warning-only. A successful import/new/open
clears the old destination; cancelling any of these leaves it intact.

Native VFS adapters now return true byte counts for RAM writes, preserve old RAM
files on OOM, honour explicit zero-length writes, report delete results correctly,
and stop KvFS enumeration at its actual EOF. Empty KvFS reads return zero and
partial underlying full-file reads are errors, not uninitialised output.

Reproduce on a normal image with `python3 tests/boot/save-as-smoke.py`; after
building `STORAGE_LAB=1 iso`, use `GUI_SAVE_LAB=1` and select either EHCI/GPT or
UHCI/MBR via the same environment variables as the FAT tests above. Both kernels
run by default. The lab tests drive real mouse widgets, write 65535 bytes from
an actual editor, exercise named/folder/UTF-8/overwrite/Undo/Redo, cold GUI import,
and detach USB after the GUI write reaches an observed directory-sector cut.
The interrupted handler must return error with text/history intact; a fresh
VM recovers before mount and external namespace/content/fsck checks must pass.
The `fattest arm` pause is lab-only and does not grant write permission by itself.
Writes are synchronous: large lab UHCI saves can block GUI handling until I/O
returns. The test waits up to 60 seconds for one measured storage click (normal
input clicks keep their 8-second budget); it never retries the press blindly.

The installed BIOS loader now uses a deterministic tar memdisk, gzip kernel and
prelinked GRUB modules instead of duplicate module files. All menu entries/apps
remain present. Structural payload checks and actual installed-disk boot are
required; the change does not compress the ISO/UEFI kernel or enable lab setup.

### M2.4 — normal owned-file WAL (supersedes the earlier normal-RO-only policy)

Normal kernels now contain the durable STX implementation. **Every FAT mount is
RO by default**, including legacy ATA/FAT16. Native RAM/KvFS are unchanged.
Only a prepared FAT32 volume can explicitly opt in to ordinary writes:

```text
mount
fatwrite usb0p2 enable
# GUI: Save as -> Destination... -> volume/folder/name, confirm replacement
fatwrite usb0p2 disable
```

Use the actual name reported by `mount`; never assume every flash drive is p2.
Rescan/reconnect/reboot restores RO and invalidates old editor destinations. The
transport lease is issued only inside recovery/write, checked against the USB
namespace generation, bounded to the FAT filesystem, and revoked on every return.
There is no shell command granting raw whole-device writes. GPT/MBR/MSR/backup
partition tables and gaps are never modified for a save. NTFS remains RO.

#### Provisioning and ownership

`tools/fat-wal.py` is a Linux/Python tool requiring mtools, dosfstools and `cp`.
It NEVER formats, repartitions, resets an existing log or repairs a filesystem.
Default invocation is read-only inspection. A private raw FAT32 partition IMAGE:

```sh
python3 tools/fat-wal.py disposable-fat-partition.img
# Use exactly the 8-hex-digit serial printed by inspection:
python3 tools/fat-wal.py disposable-fat-partition.img --prepare --confirm 12345678
```

For regular disk images only, `--start SECTOR` selects the already known FAT
partition offset. Physical access additionally requires `--allow-block-partition`,
Linux sysfs proof that the target is a PARTITION (never a whole disk), no mounted
parent/target/dependent mapper or swap, exclusive block open, 512-byte logical
sectors and an exact serial confirmation. No physical path was invoked here.
These guards are software/mock-tested, NOT validation of real Linux block-device
O_EXCL/ioctl behavior or of a particular USB controller/device. **Use only a
spare disposable flash drive with an independent backup for hardware acceptance.**

The tool snapshots the filesystem to private sparse staging files, runs `fsck.fat
-n`, checks mirror equality/namespace budgets, uses mtools ONLY on those private
copies, and proves the resulting contiguous file is exclusively owned. Full,
fragmented, conflicting-name and unsupported layouts are rejected without writing
the original. It verifies the original fd against the snapshot before applying
at most 4096 changed sectors; no subprocess reopens `/dev/...` for writes. Fresh
allocation data is written before metadata and existing directory publication.
The original BPB/content is rechecked while applying the plan. The validated WAL
header is synced first; the CRC-protected owner sector is committed **last**.

Temporary storage must fit two sparse partition copies; nonzero free space can
still require substantial disk space. `--temporary-directory /existing/directory`
selects storage instead of the default temporary directory. Initial host
provisioning is an unjournalled standard FAT file creation and **must not be
interrupted**. A backup is required. If preparation loses power, do not try
KvantOS RW: externally check the unready filesystem first. The kernel never
initializes/guesses a zero or corrupt owner file. Once prepared, the owner file
is durable consent to replay prior KvantOS operations automatically at mount,
including when the resulting ordinary namespace is labelled RO. New file saves
still require `fatwrite ... enable` each session.

Supported enrollment: FAT32, 512-byte sectors, exactly two mirrored FATs, a
contiguous root `KVANTWAL.SYS`, read-only+hidden+system attributes. Its exact size
is 50 sectors rounded up to a whole cluster: immutable 1-sector owner + 49-sector
STX log (48 undo records), then protected padding. Owner fields bind full BPB CRC,
volume serial, geometry, file cluster/extent, root entry location and protocol.
Directory validation is bounded to 1024 directories including root and 262144
ordinary entries; oversized/ambiguous/cyclic layouts fail closed. The complete
FAT tables are checked in 8-sector batches, not sampled. No lost/foreign chain
may enter ANY journal cluster, and no other directory entry may reference one.
FAT-chain changes, data writes, deletion/replacement and SFN/LFN aliases cannot
modify the owned file through VFS. The immutable owner entry is checked even
when another entry in its sector changes or is recovered.

The STX address space is volume-relative. A disjoint virtual journal range is
mapped only to that proven file's physical data; metadata targets may not enter
the file, BPB, boot backups or outside the filesystem. `stx_load` verifies ALL
records/addresses/CRCs without writing. Ownership is checked using an in-memory
undo overlay of the pre-operation FAT/namespace, so an explained interrupted FAT
mirror update/root extension/rmdir is recoverable. Physical journal links are
also checked. Only then is replay granted; failure withholds namespace publication.
A torn metadata word that makes physical journal ownership ambiguous may also
be conservatively refused even with valid undo data; automatic recovery from
every possible corruption/layout is not promised. The log is never reformatted
as a recovery shortcut. Normal unprepared media
and experimental lab-marker volumes receive no recovery/write permission.

#### Exclusive-media and failure-model contract

This is a standard FAT filesystem with a private KvantOS undo file, **not a FAT
journal understood by Windows/Linux**. Other OSes may edit normal files only
after a successful KvantOS completion/clean log, with no concurrent writer. Never
move, delete, defragment or edit the owned file/partition geometry while enrolled.
After ANY uncertain save, crash or unplug, **boot KvantOS for recovery BEFORE
mounting elsewhere for writes or allowing fsck repair**. A different OS cannot
safely repair a pending private WAL. Keep the device exclusively attached; the
system does not provide cryptographic ownership or arbitrary external-writer
conflict detection. Clean hints in FSInfo are not permission to skip checks.

FAT's clean-shutdown bit is cleared inside the metadata transaction and restored
before closing the durable log. This is only a best-effort warning hint: a small
commit window can have a clean FAT bit with a pending WAL. It is NOT a cross-OS
lock/recovery marker. A pre-operation dirty/error FAT, unexplained mirror mismatch,
invalid owner/header/backup or ambiguous commit is not silently repaired. Recovery
needs working media and honest durable flushes. CRC detects accidental corruption,
not malicious forgery; arbitrary bit rot, controller lies and loss of the owner
filename itself are outside the proven old/new sector-tear failure model.
Existing unrelated filesystem corruption is not a background fsck service.

The FAT driver remains synchronous and single-caller: its shared buffers/STX
are not an SMP/multithreaded filesystem-entry implementation. Current GUI/shell
serialize these operations; arbitrary concurrent kernel callers are unsupported.
Writes remain synchronous and may block the GUI. A failed/interrupted save never
sets the document checkpoint or falls back to another target. Reconnect requires
fresh namespace/generation and opt-in. The 48-record limit is checked before each
new metadata target; exhaustion aborts/fences, not an unlogged overflow. This is
not a claim that arbitrary-size operations always fit (editor limit is 65535B).

#### Reproduction and remaining acceptance

```sh
make -C tests/fs build/fat-owned-sanitize
FAT_OWNED_HARNESS="$PWD/tests/fs/build/fat-owned-sanitize" python3 tests/fs/test_fat_owned.py
python3 tests/fs/test_fat_owned_policy.py
make -j2 CC=gcc-12 STORAGE_LAB=0 iso
USB_CONTROLLER=ehci-ohci USB_LAYOUT=gpt python3 tests/boot/fat-owned-smoke.py
GUI_SAVE_OWNED=1 USB_CONTROLLER=ehci-ohci USB_LAYOUT=gpt python3 tests/boot/save-as-smoke.py
# Repeat both guest commands with USB_CONTROLLER=uhci USB_LAYOUT=mbr.
```

Guest scripts run BIOS i386 and UEFI x86-64 automatically. No host devices are
opened, no mount is performed, and no lab symbols/kernel RAM are patched. QEMU's
Unix debugger stops ordinary functions for selected real power-loss/unplug cuts;
backend blkdebug supplies read/write/flush failures. Host tests cut every
write/barrier boundary, validate namespace/content/fsck after fresh-process replay,
cover 64 KiB, empty/full files, torn sectors, repeated recovery, crosslinks and
aliases. Provisioner refusal tests also check offset GPT boundaries and simulate
Linux mount/mapper/swap gates. Hardware-specific gates are NOT physically tested.

**The software M2 path is implemented; whole M2 still awaits physical USB and
VirtualBox passthrough acceptance on disposable hardware.** Remote CI is configured,
not executed for this local branch. No production push/tag/release is implied.

Normal-kernel KvFS QEMU tests inject backend flush errors on the new body and a
write error targeting the directory sector. Both kernels preserve RAM text,
Undo/savepoint and the old on-disk file after cold reboot; metadata failure also
fences the mount. GUI test clicks wait for one actual press/release acknowledgement,
not a fixed pulse or blind retries. The tiny `gui_mouse_seen` probe acknowledges
handled input; this is not a queued OS mouse-event subsystem.
