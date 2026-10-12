# Changelog

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
