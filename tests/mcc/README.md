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
