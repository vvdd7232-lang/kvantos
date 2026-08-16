# KvantOS 0.1.0 "Photon"

*Read this in [Русский](README.ru.md).*

A completely hand-written **32-bit operating system** for the i386 architecture.
Own monolithic kernel, booted by **GRUB 2** through the Multiboot 1 specification.
No C standard library — only the compiler's freestanding headers.

```
   ██╗  ██╗██╗   ██╗ █████╗ ███╗   ██╗████████╗
   ██║ ██╔╝██║   ██║██╔══██╗████╗  ██║╚══██╔══╝
   █████╔╝ ██║   ██║███████║██╔██╗ ██║   ██║
   ██╔═██╗ ╚██╗ ██╔╝██╔══██║██║╚██╗██║   ██║
   ██║  ██╗ ╚████╔╝ ██║  ██║██║ ╚████║   ██║
   ╚═╝  ╚═╝  ╚═══╝  ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝
```

## Quick start

```bash
make          # build the kernel, build/kvant.bin
make iso      # build the bootable image, build/kvantos.iso
make run      # run in QEMU (the kernel log is mirrored to COM1/stdout)
```

The image boots on real hardware too: write `kvantos.iso` to a USB stick
(`dd if=build/kvantos.iso of=/dev/sdX bs=4M`) or attach it as a CD in
VirtualBox/VMware.

## Interface language

The whole system speaks **English by default** and can switch to **Russian at
run time** — the entire interface, shell, GUI and bundled applications.

| How | What to do |
|---|---|
| GRUB menu | pick the second entry, *KvantOS - Russian interface* |
| Kernel command line | add `lang=ru` |
| Inside the system | type `lang ru` (and `lang en` to switch back) |

The mechanism is a single macro, `T(en, ru)`, declared in `include/i18n.h`: it
picks a string from a pair at run time, so both languages live in one binary and
switching costs nothing. Applications reach the same switch through the ABI —
`sys->lang()` or the `KV_T(sys, en, ru)` macro. Every source comment, build
script and document in the repository is in English; the Russian documents are
kept beside them as `*.ru.md`.

## What is implemented

### Booting
* Multiboot 1 header, assembly entry point (`boot/boot.asm`)
* Parsing of the multiboot structure: memory map (`mmap`), bootloader name, RAM size
* A GRUB menu with nine entries, including safe mode, text mode and Russian interface

### Kernel
| Subsystem | Details |
|---|---|
| **GDT** | 5 descriptors: ring 0 and ring 3 code/data, plus a TSS for a future userspace |
| **IDT** | 32 exception handlers, 16 IRQs, vector `0x80` reserved for system calls |
| **PIC 8259** | IRQs remapped to vectors 32–47, proper EOI handling |
| **PIT** | 100 Hz system timer, the source of task preemption |
| **PMM** | A bitmap of 4 KiB physical pages built from the multiboot mmap |
| **Paging** | Identity mapping of the first 16 MiB, `CR3`/`CR0.PG`, a `#PF` handler dumping `CR2` |
| **Heap** | 10 MiB, `kmalloc`/`kfree`/`kcalloc`, first fit, splitting and merging of neighbouring blocks, magic numbers against corruption |
| **Scheduler** | Round robin with preemption on IRQ0, context switching in assembly, ready/sleeping/finished states, automatic reaping of dead tasks |
| **ramfs** | A flat in-RAM filesystem: create, read and delete files |

### Graphics mode — the `guimenu` command

A full windowing environment, **KvantGUI**, on top of the linear VBE framebuffer
requested from GRUB through the video fields of the Multiboot header (1024×768×32).

* **Desktop** — a gradient background and 9 icons with hand-drawn pixel art
* **Windows** — a gradient title bar, a shadow, a close button, dragging with the
  mouse, z-order (a click raises the window)
* **Taskbar** — buttons for open windows, an RTC clock, a memory gauge
* **PS/2 mouse** — an IRQ12 driver, the cursor drawn in software, support for
  clicks, dragging and double clicks
* **Double buffering** — a frame is composed in a heap buffer and flushed to the
  screen as a whole, so there is no flicker
* **Mini-applications**: About, Monitor (memory, heap and tasks in real time),
  Files (ramfs), Terminal (help/mem/ps/ls/date/uptime/about/clear), Paint
  (a 44×26 canvas with a palette), Settings, Help, Programs

#### The Settings application

A graphical counterpart of the `vidmode`/`refresh` commands, with three tabs:

* **Display** — a grid of available resolutions (modes that do not fit into video
  memory are hidden), a refresh rate selector and an Apply button. The resolution
  changes **while the environment keeps running**: the buffer for the new stride
  is allocated *before* the adapter is touched, windows are pulled back inside the
  screen and the cursor is re-centred. If the buffer cannot be allocated the mode
  is left untouched — the desktop is never left rendering straight into video
  memory, which on real hardware means uncached writes and an apparent freeze.
  After a successful change a banner asks for confirmation: **Enter** keeps the
  new mode, **Esc** undoes it, and after 15 seconds without input the previous
  resolution is restored automatically, so an unusable mode can never lock you
  out (`screenshots/56_revert_banner.png`, `57_auto_reverted.png`).
* **Appearance** — 6 accent colours (they repaint window titles, the Start button
  and the background), 4 wallpaper styles (gradient, night, grid, plain), plus
  checkboxes for window shadows and seconds in the clock.
* **System** — version, CPU, memory, heap, uptime, the number of tasks and PCI
  devices, and a way back to the console.

Internally there is a system of *hit areas*: every frame the widgets register
their rectangles, and a mouse click looks for a hit in reverse drawing order.

Keys: `T` terminal, `M` monitor, `F` files, `A` about, `P` paint, `S` settings,
`H` help, `G` programs, `E` close window, arrows + Enter to pick an icon,
`Esc`/`Q` to return to the text console.

### Video adapter, resolution and refresh rate

The kernel drives the video adapter itself, without asking the BIOS: `int 0x10`
is unreachable from protected mode, so the registers are programmed directly.

| Command | Purpose |
|---|---|
| `lspci` / `lspci -v` | enumerate PCI devices, classes, BARs, IRQs |
| `gpu` | video adapter model, video memory size, driver, current mode |
| `vidmode` | a table of 12 modes with a check whether they fit into video memory |
| `vidmode 6` | switch to mode #6 from the table |
| `vidmode 1280 720 32` | an arbitrary resolution and colour depth |
| `refresh` | the current rate: computed from CRTC and measured against VSync |
| `refresh 75` | set the frame rate |

**How it works.** The PCI bus is scanned through ports `0xCF8`/`0xCFC`
(256 buses × 32 slots, multifunction devices included); the amount of video
memory is determined by writing a mask into a BAR. Resolution changes go through
the **Bochs VBE Extensions** (ports `0x1CE`/`0x1CF`) — understood by QEMU, Bochs
and VirtualBox; for VMware there is a path through the SVGA II registers in BAR0.
After a switch the driver re-maps video memory into the address space,
recomputes the pixel format (BGRX for 32/24 bits, RGB565 for 16) and rebuilds the
console grid — 1280×1024, for instance, yields 160×64 characters. The `guimenu`
environment picks the new resolution up automatically.

**The frame rate** is changed by reprogramming the vertical CRTC timings:
Vertical Total, Retrace Start and Retrace End are recomputed together, in one
go. Changing only Vertical Total is not allowed — the sync pulse would stay
where it was, the signal would become invalid and the monitor would blank the
picture (this bug was caught on screenshots and fixed). In linear framebuffer
mode the scan-out is owned by the host, so the system says so honestly instead
of showing an invented number.

### Drivers
* **Console** — works in two modes: classic VGA 80×25 through 0xB8000 with its
  own font in plane 2, or software glyph rendering straight into the framebuffer
  (128×48 characters at 1024×768). The choice is automatic
* **VBE framebuffer** — 32/24/16 bits per pixel, fills, gradients, rounded
  rectangles, text, hardware scrolling of an area
* **PS/2 mouse** — IRQ12, 3-byte packets, sync-bit and overflow checks
* **Cyrillic** — a CP866 font generated from system PSF files
  (`tools/mkfont.py`); the driver decodes UTF-8 → CP866 on the fly
* **PS/2 keyboard** — scan code set 1, Shift/Ctrl/CapsLock, arrows, a ring
  buffer, handling inside the interrupt
* **RTC/CMOS** — reading date and time, BCD and 12/24-hour support
* **COM1** — every kernel message is mirrored to the serial port for debugging
* **PC speaker** — sound generated through PIT channel 2

### The kvsh shell
An interactive command line with history (↑↓ arrows), Ctrl+L and Ctrl+C.

```
help      about     clear     echo      mem       cpu
uptime    date      ps        spawn     ls        cat
write     rm        colors    beep      alloc     lang
history   crash     reboot    poweroff  guimenu   gfx
lspci     gpu       vidmode   refresh   setup     disk
format    df        dls       dcat      install   uninstall
apps      hwreport
```

`guimenu` starts the graphical environment, `gfx` reports the video mode,
`lang` switches the interface language.

A live status line at the top is updated by a separate background task and shows
RAM usage, the number of tasks and the system uptime.

## Project layout

```
kvantos/
├── boot/
│   ├── boot.asm        entry point, Multiboot, GDT/IDT/paging/context switch
│   └── isr.asm         48 interrupt stubs + the common dispatcher
├── include/
│   ├── kernel.h        the single kernel header
│   ├── i18n.h          the T() macro and run-time language switching
│   └── kvapp.h         the application ABI, version 2
├── kernel/
│   ├── main.c          subsystem initialisation
│   ├── i18n.c          the current language and its selector
│   ├── gdt.c  idt.c    descriptor tables, PIC
│   ├── pmm.c  paging.c heap.c    memory management
│   ├── sched.c         the task scheduler
│   ├── vga.c  serial.c printf.c  output
│   ├── keyboard.c timer.c rtc.c cpu.c   drivers
│   ├── fb.c            the linear framebuffer (VBE) driver
│   ├── pci.c           PCI bus enumeration, BAR reading
│   ├── vbe.c           resolution (BGA/SVGA) and CRTC timing changes
│   ├── cmd_video.c     the lspci / gpu / vidmode / refresh commands
│   ├── gui.c           the graphical environment: windows, panel, applications
│   ├── kapp.c          the .kapp application loader
│   ├── kvfs.c ata.c    the disk filesystem and its ATA driver
│   ├── setup.c         installation onto a hard disk
│   ├── mouse.c         the PS/2 mouse driver
│   ├── charset.c       UTF-8 -> CP866
│   ├── ramfs.c shell.c panic.c string.c
│   └── font8x16.c      the generated VGA font with Cyrillic
├── sdk/                the application SDK: samples, linker script, packer
├── grub/grub.cfg       the bootloader menu
├── tools/mkfont.py     the font generator
├── linker.ld           the kernel at address 1 MiB
└── Makefile
```

## Technical decisions

**Why a custom font.** The stock VGA character generator only carries CP437 —
there is no Cyrillic in it. At start-up the kernel reprograms the VGA Sequencer
and Graphics Controller, writes 256 glyphs of 8×16 into plane 2 and returns the
adapter to text mode. The sources stay in UTF-8; the driver converts them to
CP866 with a streaming decoder.

**Mapping the framebuffer.** Video memory sits around `0xFD000000`, far beyond
the identity mapping of the first 16 MiB. That is why `fb_init` only parses the
Multiboot fields, while the buffer pages are mapped one-to-one right after
paging is enabled — otherwise the very first access would raise a `#PF`.

**No libgcc.** Linking goes straight through `ld` without standard libraries, so
64-bit divisions (`__udivdi3`) are replaced by arithmetic built from 32-bit
operations — the kernel drags in no external dependencies.

**Context switching.** `context_switch` saves `EFLAGS`, `ebp`, `ebx`, `esi` and
`edi` on the stack of the old task and restores them from the stack of the new
one. The stack of a newly created task is filled with exactly the same frame, so
the first `ret` takes execution straight to its entry point, and underneath it
lies the address of `task_exit` — a task finishes correctly simply by returning
from its function.

## Licence

MIT.

## Screenshots

| File | What it shows |
|---|---|
| `screenshots/47_en_shell.png` | The kvsh shell in English: `gpu`, `df`, `lang` |
| `screenshots/48_en_help.png` | The full command list |
| `screenshots/49_en_settings.png` | The Settings application, the Display tab |
| `screenshots/50_en_programs.png` | The Programs window |
| `screenshots/51_grub_menu_lang.png` | The GRUB menu with the Russian entry |
| `screenshots/52_ru_help.png` | The same help after booting with `lang=ru` |
| `screenshots/53_ru_gui.png` | The graphical environment in Russian |
| `screenshots/54_lang_switch_runtime.png` | Switching with the `lang ru` command at run time |

Frames 01–46 cover the earlier stages: the console, the GUI, video modes,
applications and installation onto a hard disk.

## Working test screenshots

The `testshots/` folder holds 64 frames captured from QEMU while debugging:
frame-rate measurements and both runs of the notes editor, including shots of
the bugs themselves. The index is `testshots/README.md`, and the whole gallery in
one file is `testshots/index.html`. Unlike `screenshots/`, these are diagnostic
drafts rather than a showcase.

## Applications

KvantOS can **install programs onto a disk** — they survive a reboot, as in a
real system.

An application is a `.kapp` file: a flat image with a 64-byte header that the
kernel loads at a fixed address and whose `kapp_main()` function it calls. The
program receives a table of 30 system functions (drawing, keyboard, mouse, files,
time, sound) and draws inside its own window on the desktop.

The quickest start is to embed a program into the boot image:

```bash
cd sdk && make                       # build the samples
cd .. && ./sdk/addapp.sh sdk/build/snake.kapp
```

The resulting `release/kvantos.iso` can be attached to VMware/VirtualBox or
burned to a disc: the applications will be in the system straight away, **with no
hard disk at all**. A disk is only needed so that programs can save their files.

Inside the system: the **Programs** icon (key **G**) lists, launches and removes
them. From the shell: `disk`, `format`, `install`, `apps`, `dls`.

Three richly commented samples are bundled: **Clock** (an analogue dial, integer
trigonometry), **Notes** (an editor keeping its text on disk) and **Snake**
(a small game).

If an application crashes, the system removes it and keeps running — this is
verified by a dedicated test.

**The full developer guide is [APPS.md](APPS.md)**: the file format, a reference
for every ABI function, the life cycle, building, installing, debugging, an error
reference and a list of things you must not do.

## Installing onto a hard disk

KvantOS can install itself onto a hard disk and boot from it unaided — with no
USB stick and no disc in the drive.

Boot from the ISO or the floppy and run in the shell:

```
setup            # show disk information and a warning
setup --yes      # install (the filesystem is created anew)
setup --yes --keep   # install, keeping the files already there
```

The same thing in graphics mode: the **Programs** icon → the **Install** button.
The button is only visible while the system runs from removable media; after
installation it disappears.

Then run `poweroff`, remove the medium and switch the machine on — it will boot
from the hard disk.

### What happens during installation

| Disk area | Contents |
|---|---|
| sector 0 | MBR: GRUB code (446 bytes) + the partition table |
| sectors 1–2047 | the body of the GRUB bootloader (`hdboot.img`, ~386 KiB) |
| sector 2048 | the KvFS superblock |
| sectors 2049–2056 | the file directory |
| sector 2057 onwards | file data |

A single partition is created: active, type `0x83`, from sector 2048 to the end
of the disk. The kernel and the applications live inside the bootloader image
itself, so no separate service partition is needed. After writing the
bootloader the installer moves every ramfs file into KvFS — the programs stay
where they were.

**Warning:** installation overwrites the boot record of the disk and everything
previously on it becomes unreachable. In a virtual machine the disk must be
**IDE**, not SCSI.

A detailed step-by-step guide, including real hardware, is in
[INSTALL.md](INSTALL.md).

## Code audit

A full account of the defects found and fixed (74 items: video memory size
caching, heap overflows, CapsLock, the icon layout at 640×480 and more) is in
[AUDIT.md](AUDIT.md).

## Media and building

| File | Size | When to use it |
|------|------|----------------|
| `release/kvantos.iso` | ~1.4 MB | The main option: CD/DVD or USB (the image is hybrid) |
| `release/kvantos-floppy.img` | 1.44 MB | When the drive is worn out and GRUB hangs at "Welcome to GRUB!" |

Both images are **monolithic**: the GRUB modules, `grub.cfg` and the kernel
itself are embedded inside the boot image, so nothing is read from the medium
afterwards.

```
make iso      # the bootable ISO
make floppy   # a 1.44 MB floppy image
make run      # run in QEMU
```

### Verified on real hardware

Samsung RV410 (NP-RV410L): Pentium T4500, GL40 + ICH9M chipset, GMA 4500M, 2 GB
of RAM — booted from the floppy image, the kvsh console works.

### Diagnosis without a screen

- **NumLock lit up** — the kernel reached keyboard initialisation.
- **Three indicators blinked plus a beep** — the system is fully up.
- The `hwreport` command prints a summary of CPU, RAM, PCI and video mode.
