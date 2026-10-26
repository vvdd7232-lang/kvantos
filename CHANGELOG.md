# Changelog

## Unreleased — Breeze desktop

* Windows 7-inspired light chrome: tinted title gradients, subtle borders,
  white fields, blue selection, legible menus and editor/workshop dialogs.
* Default Blue accent and new procedural Breeze wallpaper; Matrix remains an
  app and wallpaper. Existing wallpaper indices and settings.cfg stay valid.
* Caption minimise button, with taskbar restore; adaptive task slots keep all
  eight windows accessible at 640x480 without covering the clock.
* Quiet taskbar (date/time only), smaller desktop signature and compact
  Appearance controls that fit the Settings window in EN and RU.
* Layout fixes: symmetric rounded corners, bounded title gradients, centred
  button labels with UTF-8 ellipsis, separated launcher columns, wrapped About
  details and paged Help at native window sizes. Paint's label no longer sits
  under colour swatches; architecture/bootloader labels follow the build target.
* Native host raster tests under ASan/UBSan, PNG snapshots and caption/taskbar
  hit-region checks. No application ABI, storage policy or file-format changes.
* This is working-tree UI work, not a published release; 3.3.0 release notes
  and downloadable images still describe the original Matrix theme.

## 3.3.0 — dark hacker theme and Matrix

* Dark interface across windows, menus, taskbar, launcher, editors and
  dialogs: graphite panels, black surfaces and a neon-green accent.
* New Matrix wallpaper, enabled by default, plus a built-in Matrix
  application with animated digital rain and a `root@kvant:~# ./matrix`
  prompt. Matrix is the eleventh desktop application.
* Seven accent colours and five wallpapers, selectable in Settings and
  persisted in `settings.cfg`.
* Green kvsh prompt; higher-contrast editor selection; dark search, replace
  and import dialogs.
* No storage policy, application ABI or file-format changes.

## 3.2.0 — smooth graphics and a friendlier desktop

* Anti-aliased text everywhere: a coverage font (DejaVu Sans Mono rasterised
  at 3x into the same 8x16 CP866 cell, 4 bits/pixel) replaces hard bitmap
  glyphs in the GUI, terminal, editors and boot console; the legacy bitmap
  font remains as the fallback when smoothing is switched off.
* New smooth primitives (Wu lines, supersampled discs, rounded rectangles
  and title gradients, soft layered shadows); windows, icons, the KVANT
  launcher and the taskbar were redrawn with them. Bold headings and a big
  smooth desktop logo.
* Usability: icon hover highlights with explanatory tooltips (also listed
  in the launcher), a first-run hint on the desktop, date next to the
  taskbar clock, and a Settings > Appearance "Smooth text" toggle.
* The kvsh status line no longer paints over the desktop every 500 ms.
* Direct-ISO builder: bounce ceiling raised to 0x8E000 (MBI stays at
  0x90000) for the larger kernel.

## 3.1.4 — protected storage, native I/O and read-only NTFS

* M6.17–M6.20: NTFS read-only adapter now zero-fills at the exact Initialized
  Size boundary inside a cluster, refuses nonzero high DWORDs in volume/file
  sizes rather than aliasing 32-bit block/VFS APIs, and is covered by private
  512/1024/2048/4096-byte logical-sector images, MBR offsets, fragmented runs,
  poisoned tails and a 4-GiB sparse-file fixture. NTFS writes/repair remain off.
* M3.1–M3.3: read-only AHCI SATA DMA and native OHCI USB mass storage; later
  opt-in SATA/USB writes share the owned-file FAT WAL. All FAT mounts default RO;
  enrolled FAT32 requires explicit per-session `fatwrite` enable and recovery
  before namespace publication. No auto-provisioning, arbitrary-media or
  physical-device qualification.
* M1.2/M2.3a/M2.4: EN/RU editor input, literal find/replace, mouse-first Save As,
  explicit RAM/KvFS persistence state and no silent target fallback; normal
  kernels accept only explicitly prepared owned-WAL FAT32 files for optional
  writes. AHCI/USB errors, stale device generations and ambiguous WAL outcomes
  fail closed.
* M5.1: bounded fixed 1D local/global arrays (including pointer elements),
  decay/indexing and unevaluated sizeof; native pointer widths preserved.
  Existing source/data/stack limits unchanged. No VLA/array initializers or
  runtime index checks; sizeof returns int in this non-ISO subset.
* M4.1: isolated ABI design and implementation gates; NOT ring3 runtime.
* M3.5d: retained EHCI fault snapshots/probes, independent FS TT model clocks,
  archived slow-image cold recovery on stock OHCI (not new TT qualification).
  Original Save64 cause remains OPEN; no timeout relaxation or WRITE retry.

* M3.5c: experimental FS BOT leaves directly below HS hubs, nearest-parent TT
  routing and per-edge speed/epoch guards. FS async QH EPS/C/MPS/TT fields;
  successful completion must retire ASS and leave no ambiguous SplitXState.
  TT faults fence the whole HC until reboot (NOT CLEAR_TT_BUFFER recovery).
  Actual-backend SS/CS host model + explicit TD-level QEMU async-TT LAB, not
  wire/timing/bandwidth/physical qualification. FS hubs/LS/periodic/multi-TT
  alternate 1 remain unsupported. Default RO/opt-in/WAL unchanged. One initial
  parallel x64 Save As refusal remains unexplained (isolated suite and archived
  on-disk cold recovery pass); stress reliability is OPEN. See M3.5c.

* M3.5b: experimental EHCI HS-only hub trees, strict hub device/default-interface
  protocols and full HS hub-descriptor checks; reset/live ancestor speed guards.
  Isolate ALL root links before address reuse (including EHCI); reject FS/LS
  descendants before EP0, retain default RO/generation/WAL guards. Multi-TT
  capability is recorded but alternate 1 is NOT activated; no TT/split support.
  Actual topology host model plus an explicit, separately built QEMU HS-only
  fixture (not stock usb-hub, not wire/physical compliance). See M3.5b limits.

* M3.5a: replace shared EHCI qTD/bounce/periodic storage with pinned private
  per-init-attempt pools, never reassigned after failed startup. Validate DMA32,
  BAR/capabilities and transfer bounds; serialize per HC; prove ASS retirement
  and QH/qTD completion before IN copyout/toggle advancement. Bound register,
  frame and firmware waits; fence failed HCs and request PCI DMA disable.
  Actual-backend ASan/UBSan model + two-QEMU-HC debugger-timed detach tests.
  Default RO/explicit opt-in/WAL unchanged. HS hubs/TT/split and physical/VBox
  qualification remain OPEN; same-HC siblings may be lost after fencing.

* M3.3: native OHCI full-speed root-port BOT storage, private per-attempt
  HCCA/ED/TD/bounce pools, firmware handoff before DMA ownership, actual ED
  retirement (not intermediate OUT CC), carry/short/ZLP handling, bounded
  failures and reboot-only fencing. Common USB/FAT/WAL/write opt-in unchanged.
  Standalone OHCI tests separate from EHCI high-speed with OHCI companions;
  no physical/VirtualBox, hubs/HID/xHCI or whole-M3 completion claim.

* M3.2: gated AHCI WRITE DMA EXT + FLUSH CACHE EXT, per-operation bounded
  generation leases and shared FAT owned-file WAL recovery before publication.
  SATA/USB share block-generation/lease routing and editor stale-target guards.
  Rescan revokes SATA grants/advances generations; I/O fault fences until reboot.
  Link/PHY latches preserved across command setup; late DMA memory remains pinned.
  Normal MBR/GPT crash/EIO/RO/full + GUI timeout/late-close and mixed SATA/USB
  regressions use disposable images, not physical/VBox qualification. Ordinary
  mounts remain RO, no automatic provisioning, no AHCI installer or hotplug.

* M3.1: AHCI SATA read-only DMA on both kernels; firmware handoff and all-port
  engine retirement before enabling bus mastering, pinned per-port buffers,
  bounded errors/timeouts and reboot-only fencing. Stable USB IDs preserved;
  raw/MBR/GPT FAT VFS and both editor imports, ATA/KvFS + USB WAL coexistence.
  Initial no-write/no-recovery restriction superseded by M3.2 above; installer
  still ATA-only. Host-model and disposable QEMU checks are not hardware
  qualification. M2 physical USB/VirtualBox acceptance deferred, not completed.

* M2.4: NORMAL kernels support an explicitly preallocated `KVANTWAL.SYS` file,
  immutable BPB/geometry/extent ownership binding, full FAT-reference/directory
  alias checks through a validated undo overlay, and recovery BEFORE VFS mount.
  All ordinary FAT mounts now default RO, including ATA/FAT16; only enrolled
  FAT32 can opt in per session via `fatwrite VOLUME enable|disable`.
  USB write leases are bounded, generation-checked and revoked on return/error.
* BIOS floppy: compact prelinked GRUB memdisk with gzip ELF; retain all three
  bare-system menus below core size cap; structural + actual FDC boot checks.
* Added a fail-closed Linux provisioner: no format/GPT/gap writes, no existing-log
  resets; mtools works on private staged copies, deltas use the original open fd.
  Physical access is separately gated to unmounted partition devices; no actual
  physical-device qualification has been performed. New normal-policy sanitizer,
  QEMU debugger power-loss, GUI/unplug/recovery/fsck suites and CI jobs are local
  software evidence, NOT proof of safety on arbitrary hardware or external writers.

* M2.3a: shared mouse-first Save As destination browser for both editors
  (Save as -> Destination...), explicit volume/folder, file replacement consent,
  bound subsequent Save, temporary RAM versus persistent target/RO/error status.
  A selected-only save never silently falls back to RAM or writes another volume.
  Namespace/device generations prevent rebinding a stale destination after rescan.
* Fixed native VFS adapters: RAM byte-count/delete/empty-write contracts and
  allocate-before-replace; KvFS directory EOF, empty reads and short-read failure.
  Added ASan/UBSan adapter contracts and GUI save/full/RO/cold-read/unplug tests.
* Installed BIOS GRUB now links its modules once and embeds a deterministic small
  memdisk with gzip kernel, menu and all apps, avoiding the fixed core-size cap.
  ISO/UEFI kernels stay plain ELF; lab images remain non-installable/unpublishable.
  Physical USB writes still require production journal ownership and hardware tests.

* M2.2b: integrated persistent FAT WAL into an explicitly enrolled disposable
  USB-image lab: recover before VFS publication, two disjoint FAT/WAL grants,
  geometry/serial/BPB/manifest CRC binding, enumeration-generation fencing and
  namespace refusal on replay/CRC failures. Unmarked USB stays RO; normal
  kernels also withhold marked experimental volumes rather than read dirty data.
* Added actual guest USB/VFS file + crash/EIO/unplug/reconnect/full/fsck matrix
  for BIOS/UEFI, UHCI/MBR and EHCI+OHCI/GPT. This is not physical-media support:
  arbitrary-drive journal provisioning remains open; editor selection is added in M2.3a.
* Lab ISO is now deliberately non-installable (installer entry points disabled,
  no hdboot module), immediate lab boot, logs-only CI artifacts. Normal release
  guards, installed boot and the ordinary menu are preserved.

* M2.1 groundwork (not complete FAT32 saving): KvFS body copy-on-write, a single
  directory-sector publication, mount fencing on ambiguous metadata failures,
  mount extent/name/overlap validation. v2 file_count is advisory and rebuilt
  from directory entries; compatible with existing kernels and sdk/mkdisk.py.
* ATA completion/flush errors now propagate. Added blk_sync and a tested BOT
  transaction layer with IN/OUT direction, strict CSW validation and no write replay.
* Implemented USB WRITE(10)/SYNCHRONIZE CACHE(10), REQUEST SENSE, cache invalidation
  and permission revocation on error/disconnect/rescan. Normal builds cannot grant
  USB writes. USB FAT/NTFS mounts remain read-only.
* Separate STORAGE_LAB=1 diagnostics require one marked disposable image and a
  bounded scratch interval outside its filesystem partition. EHCI+OHCI/GPT and
  UHCI/MBR tested on BIOS/UEFI with backend write/flush EIO, RO media and disconnect.
  Laboratory images are not release artifacts; PR job uploads logs only.
* M2.2a: FAT COW retains the original chain until publication, with bounded
  metadata undo, all mirrored FAT copies, primary/backup FSInfo, fragmented LFN
  slots/deletion, read-error propagation, geometry/cycle/RO/name validation and
  safe 8.3 collision handling. 512/4096-byte FAT16 regressions also pass.
* Added a CRC-protected durable undo journal prototype for explicitly provisioned
  host images ONLY: 1048 interrupted real-FAT/VFS operations + external fsck and
  namespace/content checks; cache-loss/recovery, full/empty/RO and torn-sector tests.
  Normal kernels retain RAM undo only, not persistent FAT crash recovery;
  USB FAT remains RO. Real-media journal ownership/reconnect recovery, guest FAT
  write/fault/reboot tests were subsequently added in M2.2b, editor selection in M2.3a.
* Strip comments/blanks only from the generated installed-GRUB configuration to
  preserve every menu/app while leaving more headroom under the i386-pc core cap.

* Added a pure set-1 keymap with EN/RU, Alt+Shift, Caps/Shift and independent
  modifier state. Events separate Unicode, legacy commands and physical keys;
  text editors/search use UTF-8 while shell, filenames and app ABI stay compatible.
* Added mouse-first literal UTF-8 find/replace to both editors: forward/backward,
  wrapping, replace one/all; all replacements form one bounded Undo operation.
  127-byte query/replacement fields, explicit no-match/limit errors, atomic failure.
* Added persistent per-revision RAM/KvFS status and an explicit Keep: KvFS offer
  after import, including diskless and disk-full failure paths; no automatic writes.
* Added ASan/UBSan decoder/search differential tests and both-kernel QEMU coverage,
  including 640x480, clipboard, saved markers, no-disk/full-disk status, reboot and
  installed BIOS disk boot. Test input now waits for actual guest consumption and
  modifier release instead of assuming a fixed HMP delay is enough.
* Trimmed unused installed/floppy GRUB commands to stay under i386-pc's core cap;
  added the new keymap header to both build dependency rules.
* Prepared PR QEMU input/find/storage checks in an isolated Python environment;
  remote Actions have not been run for this local branch.

* Added ROADMAP.md with staged goals, acceptance criteria and honest boundaries.
* Replaced one-step toggling with independent 64-entry Undo/Redo per editor,
  bounded to 128 KiB of deltas. Separate mouse buttons and Ctrl+Z/Ctrl+Y,
  redo-branch invalidation, saved-revision tracking and load/import reset.
* Kept atomic failure and the 128-line warning; history eviction never blocks
  edits. Added saved-marker, full-byte-budget, branch, eviction and random tests
  plus both-kernel GUI acceptance for independent histories and persistence.
* Prepared PR CI boot smoke for BIOS/UEFI, model sanitizers, RAMFS/OOM and
  image validation, with read-only token permissions and no persisted checkout
  credentials. Local equivalents pass; remote CI has not been run for this branch.
* Distribution archive naming now follows KV_VERSION, avoiding dev archives
  being named after an older stable release.


## 3.1.3 — USB import, GPT and non-blocking line warning

* Added read-only USB bulk storage (UHCI and high-speed EHCI), FAT16/FAT32
  discovery and mouse-first Import in Notepad and C Workshop. UTF-8 BOM,
  CRLF, tabs, editable local names and overwrite/unsaved-change guards.
* Added bounded GPT parsing with header/array CRCs, backup fallback and
  preserved partition-slot names; no formatting, repair or source-drive writes.
* Removed the 128-line editor ceiling. Above 128 rows an amber warning is
  non-blocking. Packed text has a <64-KiB byte budget, with the existing
  120-byte line width. Native compiler source remains limited to 16000 bytes;
  larger documents may still be edited/saved, with explicit Build diagnostics.
* Cached longest-line width and copied only live text/indexes; widened the
  gutter for five-digit line numbers. Kept independent documents, UTF-8,
  clipboard, undo/redo, KvFS persistence, BIOS/i386 and UEFI/x86-64.
* Added 23 partition checks and full USB 2.0 GPT/MBR import/edit/build/run,
  hotplug, reboot/persistence and long-document tests on both architectures.
  Source-image SHA-256 hashes stay unchanged. Physical hardware/VirtualBox
  have not been independently tested; OHCI/xHCI/hubs remain unsupported.


## 3.1.2 — mouse-first editors

* Removed single-letter desktop launch/close/quit shortcuts; added single-click
  icons and a KVANT applications/exit menu. In-game keys are unchanged.
* Added mouse caret positioning, UTF-8 drag selection with edge auto-scroll,
  shared clipboard, independent one-step undo/redo and clickable editing tools.
* Added RAM/KvFS open/save-as pickers, editable filename caret, overwrite/delete
  confirmation and unsaved-change guards for new/open/close/both exit paths.
* Added working draggable vertical/horizontal scrollbars, IntelliMouse wheel
  negotiation with plain PS/2 fallback, and optional NumPad MouseKeys.
* Notepad now saves to mounted KvFS as well as RAM. Bounded text-model edits and
  loads are atomic: limits or binary input never silently truncate the document.
* Kept dirty rendering and both native compiler backends. Added sanitizer/model
  tests (25000 edits) and full measured mouse workflows on BIOS/i386 and UEFI/x64,
  including real disk persistence, Russian UI and small-screen layouts.
* Published separately as v3.1.2; older release tags and assets remain unchanged.

## 3.1.1

* Added the in-system C Workshop: desktop icon/B, independent source editor,
  F5 build/F6 rebuild-and-run, source/app persistence to RAM and KvFS.
* Added a native x86-64 backend beside i386: pointer-width memory operations,
  SysV event wrappers, aligned API/variadic calls and architecture-tagged apps.
* Added atomic RAMFS replacement and shared safe .kapp window launching.
* Discarded unused kernel unwind metadata to fit the embedded HDD installer;
  direct boot modules now start beyond the kernel BSS.
* Added bilingual guides, 73 execution tests and 40000 sanitizer mutations per
  target, plus full guest compilation/run/reboot/diskless/ABI tests on both.

## 3.1.0

### Release validation and boot fixes

* Fixed EFI PE layout, base-relocation metadata and zero-initialized globals.
* Corrected EFI memory types and GOP enumeration; claim kernel pages before
  copying and keep boot metadata/modules within the initial identity mapping.
* Removed the EFI console call after ExitBootServices.
* Fixed direct BIOS loading: absolute CD LBAs, 2048-byte sectors, copy-table
  address/counter, register preservation and safe real-mode stack placement.
* The main ISO now carries a GRUB disk bootstrap and GPT ESP, tested as USB.
* Updated the GRUB version and included all eleven application modules.
* Added image structure, dual-kernel GUI/application and USB boot regressions,
  SHA-256 manifests, bilingual release notes and a tag-driven release workflow.

### Graphics and timing

* The desktop renderer now tracks dirty rectangles, restores the static
  wallpaper/icon layer from a cache, redraws only intersecting windows and
  presents only damaged regions. Cursor movement no longer repaints the whole
  desktop; the idle GUI sleeps between events.
* KvApp ABI 3 appends `delta_ms()` while preserving the ABI-2 layout and runtime
  compatibility. The 500 Hz timer and elapsed-time simulation keep Pong, Snake,
  Life and Stars steady across the 30/60 fps and uncapped render modes.

### Applications and boot images

* Added **Maze 3D**, a fixed-point, integer-only first-person maze with DDA
  ray-casting, wall shading, collision detection and a minimap.
* The SDK now ships eleven applications for both 32-bit and 64-bit kernels.
* The GRUB-free direct ISO now carries BIOS and UEFI boot entries. The release
  archive includes it alongside the standard hybrid ISO and floppy image.
* Version bump: the kernel reports **3.1.0**.

## 3.0.0 "Horizon"

### Architecture: the move to 64-bit

* **Second, 64-bit kernel build.** The whole C kernel compiles for
  x86-64 long mode (`-m64`, no red zone, no SSE) and links into
  `build/kvant64.bin` through `linker64.ld`. The 32-bit `kvant.bin` is
  untouched: both kernels share the same sources, split where the
  hardware demands it:
  - `boot/boot64.asm` - the 64-bit entry, GDT/IDT loading, CR3 helpers
    and a new `context_switch` frame (`r15..rbx, rbp, rflags`);
  - `boot/isr64.asm` - exception/IRQ stubs with the full `r8..r15`
    save set and a 16-byte aligned stack for the C handlers;
  - `kernel/paging.c` - a PML4/PDPT/PD hierarchy with 2 MiB identity
    pages for the first 128 MiB and 4 KiB walks for everything else
    (framebuffer mapping included); page zero stays unmapped;
  - `kernel/gdt.c` - a long-mode GDT with the `L` bit and a 64-bit TSS
    (`rsp0` instead of `esp0`);
  - `kernel/idt.c` - 16-byte IDT entries, 10-byte IDTR;
  - scheduler, setjmp-based application guard, panic register dump,
    flags snapshot helpers - all carry 64-bit variants behind
    `#ifdef __x86_64__`, with `kv_addr_t` as the pointer-sized integer.
* **Applications for both kernels.** The SDK now builds every sample
  twice: the i586 `.kapp` set (`release/apps`) and an x86-64 set
  (`release/apps64`, built with `-mno-red-zone -mno-sse`). A new
  header flag, `KAPP_FLAG_ARCH64`, marks the bitness, and each kernel
  refuses to run applications built for the other one with a readable
  message instead of a crash.

### UEFI loader

* **`boot/kvantefi.c` - a UEFI boot application in plain C.** Hand-written
  minimal EFI headers (`include/efi/efi.h`), built position-independent
  and converted to PE/COFF with `objcopy --target=efi-app-x86_64`.
  The stub:
  - finds the boot volume through the Loaded Image / Simple File System
    protocols and reads `/boot/kvant64.bin` (parsed as ELF64, segments
    placed exactly where the linker wants them, at 1 MiB);
  - loads every `/boot/apps/*.kapp` and hands the files over as
    Multiboot-1 style modules - the application launcher works on UEFI
    machines too;
  - sets a 1024x768 (or best-fitting) GOP mode and describes the
    framebuffer in the `multiboot_info`;
  - converts the EFI memory map into the Multiboot `mmap` format,
    reserving the kernel, module and table ranges, and computes
    `mem_upper`;
  - exits the boot services (with the map-key retry loop the spec
    requires) and jumps into the kernel with `rdi = 0x2BADB002`,
    `rsi = multiboot_info` - from there the 64-bit KvantOS is the same
    system as the 32-bit one.

### Images

* **Hybrid ISO.** `make iso` now writes both El Torito entries: the
  BIOS/GRUB one and the UEFI one. The EFI entry boots `esp.img`, a
  FAT16 ESP built by the new `tools/mkesp.py` (pure Python, with VFAT
  long names for `*.kapp`) that carries `EFI/BOOT/BOOTX64.EFI`,
  `boot/kvant64.bin` and all ten applications. The same files are
  duplicated into the ISO9660 tree for USB installs, and `isohybrid
  --uefi` marks the stick bootable on EFI firmware.
* **`make iso-direct`** (the GRUB-free fallback) gained the UEFI entry
  as well.
* The release archive is now `kvantos-3.0.0-horizon.tar.gz` and also
  ships `kvant64.bin`, `kvantefi.efi` and `esp.img`.
* Version bump: the kernel reports **3.0.0 "Horizon"**.

## 2.0.0 "Quantum"


### System

* **Setting the clock.** The CMOS real-time clock is no longer read-only:
  `time set HH:MM:SS` and `date set DD.MM.YYYY` write the clock and the
  calendar back to the RTC, honouring the BCD/binary format the chip is
  configured for and validating ranges (leap years included).
* **Persistent settings.** Choices made in a session now survive the
  reboot: they are stored as a plain `settings.cfg` on the KvFS disk and
  loaded right after the disk is mounted. The interface language
  (`lang ru` / `lang en`) is the first persisted setting; the store is
  ready for more keys. Without a disk everything degrades gracefully.
* Version bump everywhere: the kernel reports **2.0.0 "Quantum"**
  (`about`, `uname`), the boot menu and the release archive are renamed
  accordingly (`kvantos-2.0.0-quantum.tar.gz`).

### Shell (kvsh)

New text utilities, all working on ramfs files:

* `sort FILE` - print the lines of a file sorted;
* `uniq FILE` - collapse adjacent duplicate lines;
* `tac FILE` - print a file line by line, backwards;
* `basename PATH` / `dirname PATH` - split a path;
* `repeat N TEXT` - print text N times (capped at 200).

### Applications

Ten applications instead of eight:

* **Mines** - Minesweeper: 16x12 field, 24 mines, flags, a keyboard
  cursor for mouseless machines, flood opening of empty areas;
* **2048** - the sliding-tiles puzzle with score and best-score tracking.

### Boot images

* The ISO build no longer fights the 0x78000-byte core image limit of
  i386-pc: the kernel, the menu and the applications live on the ISO as
  ordinary files while every GRUB module stays embedded in core.img.
* A GRUB-free fallback appeared: `make iso-direct` builds a bootable ISO
  with a tiny self-contained Multiboot loader (boot/direct.asm) and the
  vendored pure-Python pycdlib - useful where the GRUB toolchain is
  unavailable.

## 0.1.0 "Photon"

The first public release: the kernel, the kvsh shell, the graphics
desktop with windows and a file manager, KvFS/FAT32/NTFS support, eight
applications and installation onto a hard disk (`setup`).
