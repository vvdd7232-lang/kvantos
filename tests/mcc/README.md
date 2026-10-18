# C Workshop regression tests

`make check` executes 73 generated-code cases per backend in Unicorn. Install
`unicorn` with pip first. The x86-64 suite places the stack above 4 GiB and
checks SysV API-call alignment plus callee-saved registers.

`make sanitize ramfs editors` runs 40000 deterministic source mutations per backend
under ASan/UBSan, verifies atomic RAM-file replacement on out-of-memory and
checks the bounded text model with 25000 edits, UTF-8, undo/redo and failure preservation.

After `make CC=gcc-12 release`, run `python3 tests/mcc/smoke.py`. This boots
both BIOS/i386 and UEFI/x86-64 and checks:

- Actual in-guest compilation, RAM and KvFS save, native drawing and clicks.
- Independent Workshop and Notepad documents, compile errors without stale run.
- Disk contents inspected on the host and source reopened after full reboot.
- Native locals, double pointers, heap allocation, eight-argument functions,
  and a variadic format call with register and stack arguments.
- Russian diskless build/run.

The only written disks are private `build/{bios32,uefi64}/workshop.img` files
under this directory. UEFI uses a separate PIIX3 legacy IDE controller because
q35's built-in disk controller is AHCI, unsupported by the OS. The harness
asserts KvFS really mounted; an unchanged template alone is not considered
proof of persistence. Pointer input uses bounded PS/2 movements with actual
guest coordinate feedback, not blind mouse retries.

`python3 tests/boot/editor-smoke.py` (from repository root) covers the mouse-first
editor acceptance flows on both kernels, using a separate private disk.

## EHCI DMA prerequisite (M3.5a)

`make -C tests/mcc CC=gcc-12 ehci` from the repository root compiles the **actual**
EHCI backend with a simulated MMIO/DMA model under ASan/UBSan (`-no-pie` keeps
model DMA addresses below 4 GiB). Checks private per-attempt leases, failed-init
slot reuse and simulated late DMA; control/BOT/short/ZLP toggles, retirement,
no-copy/no-toggle errors, malformed writeback, stuck schedules and frozen TSC.
This is a model, not hardware/TT qualification.

After a normal `make CC=gcc-12 release`, run
`python3 tests/boot/ehci-isolation-smoke.py`: BIOS32 and UEFI64 each use two real
QEMU HS root controllers. An external debugger breakpoint times USB detach
between the link check and schedule-off. No product hooks or guest RAM edits:
HC0 is fenced, the RAM destination remains intact, HC1 still reads >16 KiB,
and source-image hashes stay exact. See `docs/STORAGE-M3.5A.md` for the limits.

## HS-only hub stage (M3.5b)

The `hub` target now also checks the actual topology code with HS nested hubs,
single/multi protocol descriptors (alternate 0 only), live speed loss, refusal
of FS/LS before any child EP0 and malformed descriptor cases. No split model.
For guest HS integration use `tests/boot/build-hs-qemu.sh`, explicit
`USB_HS_LAB=1`, and `QEMU_SYSTEM_X86_64` pointing at that separate binary.
The ordinary QEMU `usb-hub` stays FS-only. See `docs/STORAGE-M3.5B.md`; do not
report these model results as physical HS/TT or same-HC recovery qualification.

## Experimental FS async TT (M3.5c)

`make -C tests/mcc CC=gcc-12 usb-layout hub ehci` compiles the actual production
struct layouts for i386/x64 NORMAL+LAB, topology code and EHCI backend under
ASan/UBSan. Host checks distinguish SS NAK/ACK, CS NYET/result and permanent
busy/NYET; faults/ASS/ambiguous SplitXState retain caller/toggle and fence the
whole HC, including subsequent HS transfers. Topology uses the nearest HS
parent/physical port, single+multi-TT descriptors in alt0, and per-edge speed.

`tests/boot/build-tt-qemu.sh` builds a separate TD-level SS/CS fixture, NOT a
physical USB2 TT. See `docs/STORAGE-M3.5C.md` for explicit LAB env and native
GUI/import/save/WAL scope. `ehci-tt-guard-smoke.py` additionally observes a live
FS QH; wrong-route case **deliberately modifies one QH via debugger**, while
in-flight disconnect removes the emulated FS device. Neither is hardware proof.

`ehci-tt-recovery-smoke.py PROOF_DIRECTORY` replays the archived private image
from the observed initial x64 large-save refusal. It checks cold recovery and
old-or-complete-new namespace on both kernels, default RO, fsck and outside-
partition hashes. It does not reproduce/explain the original timing failure;
that observation remains OPEN even when isolated native suites pass.
