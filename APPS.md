# KvantOS applications — the complete developer guide

*Read this in [Русский](APPS.ru.md).*

Interface version: **KvApp ABI 2**
File format: **`.kapp`**, version 1

This is an exhaustive description of how KvantOS applications are built and how
to write your own. You do not have to read it end to end: if you want a result
straight away, start with "In five minutes" and come back to the rest when you
need it.

---

## Contents

1. [In five minutes: your first application](#in-five-minutes-your-first-application)
2. [How it all works](#how-it-all-works)
3. [The .kapp file format](#the-kapp-file-format)
4. [The application life cycle](#the-application-life-cycle)
5. [System function reference](#system-function-reference)
6. [Keyboard and mouse](#keyboard-and-mouse)
7. [Working with files](#working-with-files)
8. [Interface language](#interface-language)
9. [Building](#building)
10. [Installing into the system](#installing-into-the-system)
11. [What you must not do](#what-you-must-not-do)
12. [Debugging](#debugging)
13. [Error reference](#error-reference)
14. [Examples](#examples)

---

## In five minutes: your first application

Create the file `sdk/apps/hello.c`:

```c
/* KAPP_NAME "Hello" */
#include "kvapp.h"

static const kv_api_t *sys;      /* the system function table */
static int clicks = 0;

static void on_draw(void) {
    sys->clear(sys->rgb(240, 244, 250));
    sys->text(20, 20, "Hello, KvantOS!", sys->rgb(20, 30, 50), 0xFFFFFFFF);

    char buf[64];
    sys->format(buf, sizeof(buf), "Clicks: %u", clicks);
    sys->text(20, 50, buf, sys->rgb(120, 130, 150), 0xFFFFFFFF);
}

static void on_click(kv_i32 x, kv_i32 y, kv_i32 button) {
    clicks++;
    sys->beep(880, 30);
}

static kv_app_t me = {
    .title    = "Hello",
    .width    = 300,
    .height   = 160,
    .on_draw  = on_draw,
    .on_click = on_click,
};

kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    return &me;
}
```

Build it and put it on a disk:

```bash
cd sdk
make                                    # produces build/hello.kapp
python3 mkdisk.py ../disk.img 64 build/hello.kapp
```

Start the system and open the **Programs** icon (or press **G** in graphics
mode) — the application will be in the list. Clicking the row launches it.

That is a complete program: 300×160 pixels, its own mouse handler, sound. It
weighs about a kilobyte.

---

## How it all works

A KvantOS application is a **flat binary image** that the kernel loads at a fixed
address and in which it calls a single function. No ELF at run time, no dynamic
libraries, no relocation tables.

```
   ┌─────────────────────────────────────────────┐
   │  The file program.kapp on disk (KvFS)       │
   │  ┌────────────┬──────────────────────────┐  │
   │  │  header    │  memory image            │  │
   │  │  64 bytes  │  code + data             │  │
   │  └────────────┴──────────────────────────┘  │
   └──────────────────┬──────────────────────────┘
                      │  1. read from disk
                      │  2. check the signature and the ABI version
                      ▼
   ┌─────────────────────────────────────────────┐
   │  Memory at address 0x00E00000 (14 MiB)      │
   │  code + data + zeroed bss                   │
   └──────────────────┬──────────────────────────┘
                      │  3. call kapp_main(&api)
                      ▼
   ┌─────────────────────────────────────────────┐
   │  The application returns a kv_app_t         │
   │  with the on_draw / on_key / … handlers     │
   └──────────────────┬──────────────────────────┘
                      │  4. the shell calls them every frame
                      ▼
              a window on the desktop
```

### Why a fixed address

A program is built for the address `0x00E00000` from the start, so the kernel has
nothing to relocate — it copies the bytes and transfers control. The price of
that simplicity: **only one application runs at a time**. For a system of this
size that is a fair trade; launching a second one closes the first.

### Which ring an application runs in

In the **kernel ring (ring 0)**. This is a deliberate decision: the application
gets direct access to the system functions with no ring-switch overhead, and the
ABI stays simple — ordinary function calls through a table of pointers.

The flip side is that an application can technically damage the system. That is
why every entry into a program is wrapped in a **safety net**: if a CPU exception
happens inside the application (a bad address, a division by zero, an invalid
instruction), the kernel does not panic — it removes the culprit and returns to
the shell. The window then shows "Application not loaded" and the reason.

Verified in practice: an application writing to address zero is closed while the
system keeps running — the frame counter ticks on and the shell answers keys.

The zero page of memory is deliberately left unmapped, so that dereferencing
`NULL` raises an honest exception instead of silently corrupting the interrupt
table.

---

## The .kapp file format

The file consists of a 64-byte header and a memory image. All numbers are
little-endian.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `magic` | Always `KAPP` |
| 4 | 2 | `version` | Format version, currently 1 |
| 6 | 2 | `header_size` | Header size, 64 |
| 8 | 4 | `api_version` | The ABI version it was built against |
| 12 | 4 | `flags` | 1 — a windowed application |
| 16 | 4 | `load_base` | Load address, must be `0x00E00000` |
| 20 | 4 | `entry` | The absolute address of `kapp_main` |
| 24 | 4 | `code_size` | How many bytes to read from the file |
| 28 | 4 | `bss_size` | How many bytes to zero afterwards |
| 32 | 32 | `name` | The desktop title, UTF-8 |

Limits:

* code + bss must not exceed **2 MiB**;
* the entry point must fall inside the image;
* on an `api_version` mismatch the kernel refuses to run the file and says
  plainly which version it needs.

The header is produced by `sdk/mkkapp.py` — you never assemble it by hand.

---

## The application life cycle

```
   launch (a click in Programs)
        │
        ├─► kapp_main(api)     once. Save api and return a kv_app_t
        │
        ├─► on_open()          once, the window already exists
        │
        │   ┌──────────── every frame while the window is open ────┐
        ├──►│  on_tick()   calculations, animation, time polling   │
        │   │  on_draw()   drawing                                 │
        │   └──────────────────────────────────────────────────────┘
        │
        ├─► on_key(code)       when the window is active and a key is pressed
        ├─► on_click(x,y,btn)  a click inside the client area
        │
        └─► on_close()         the window is being closed
```

Any handler may be omitted — leave the field zero.

**Important:** the `kv_app_t` structure returned by `kapp_main` must live for the
whole run of the program. Declare it `static`, otherwise you return a pointer to
a local variable that has already vanished.

A frame is drawn from scratch every time. The kernel fills the window background
itself, but if the application does not call `clear()` the previous frame stays
underneath — which is usually exactly what animation wants.

---

## System function reference

Everything an application can ask of the system lives in the `kv_api_t`
structure. A pointer to it arrives in `kapp_main` — store it in a global.

Coordinates are **always** measured from the top-left corner of the window's
client area. Anything outside is clipped by the kernel: spoiling another window
or the taskbar is impossible.

### Canvas size

| Function | Description |
|---|---|
| `kv_i32 width(void)` | Client area width in pixels |
| `kv_i32 height(void)` | Client area height |

The size may differ from the one requested in `kv_app_t` if the window did not
fit on the screen. **Do not cache these values** — ask every frame.

### Drawing

| Function | Description |
|---|---|
| `kv_u32 rgb(r, g, b)` | Compose a colour from components 0…255 |
| `void clear(color)` | Fill the whole window |
| `void pixel(x, y, color)` | A single dot |
| `void fill(x, y, w, h, color)` | A filled rectangle |
| `void rect(x, y, w, h, color)` | A 1-pixel frame |
| `void line(x0, y0, x1, y1, color)` | A line segment |
| `void text(x, y, s, fg, bg)` | A UTF-8 string, Cyrillic allowed |
| `kv_i32 text_width(s)` | String width in pixels |

For a transparent text background pass `bg = 0xFFFFFFFF`.

The font cell is 8×16 pixels, constants `KV_CHAR_W` and `KV_CHAR_H`. There is no
line wrapping: a `\n` inside a string is not handled, split the text yourself.

### Status line

| Function | Description |
|---|---|
| `void status(s)` | Text in the line at the bottom of the window |

The system draws that line; the application does not have to reserve space.

### Memory

| Function | Description |
|---|---|
| `void *alloc(size)` | Allocate memory in the kernel heap |
| `void release(p)` | Free it |

The heap is shared with the kernel, 10 MiB. Release what you take: unloading an
application does not return its memory automatically.

Small buffers are easier to keep in static arrays — they land in bss, which the
kernel zeroes for you.

### Time and sound

| Function | Description |
|---|---|
| `kv_u32 ticks(void)` | Timer ticks since boot |
| `kv_u32 hz(void)` | Timer frequency (currently 1000) |
| `kv_u32 seconds(void)` | Seconds since boot |
| `void clock(&h, &m, &s)` | The real-time clock |
| `void beep(freq, ms)` | Sound through the PC speaker, no longer than 1 s |

A tick equals a millisecond, but **do not rely on that** — ask `hz()`. That is
exactly how the animation in the Notes sample works:
`(ticks() - mark) / (hz() / 2)` gives half-second intervals regardless of the
timer settings.

### Strings and memory

An application has no standard library, so the essentials are in the ABI:

| Function | Description |
|---|---|
| `void *mem_set(d, c, n)` | Fill with a byte |
| `void *mem_copy(d, s, n)` | Copy |
| `kv_u32 str_len(s)` | String length in bytes |
| `kv_i32 str_cmp(a, b)` | Compare |
| `void str_copy(d, s, max)` | Copy with a limit |
| `void format(buf, size, fmt, …)` | Compose a string |

`format` understands `%d`, `%u`, `%x`, `%s`, `%c`, `%%` and zero-padded widths
(`%08x`). **There is no left alignment (`%-10s`)** — pad with spaces by hand.

### Miscellaneous

| Function | Description |
|---|---|
| `kv_u32 random(void)` | A pseudo-random number |
| `void log(s)` | A line into the Terminal window |
| `kv_i32 lang(void)` | The current interface language: 0 English, 1 Russian |

`log` is the main debugging tool, see below.

---

## Keyboard and mouse

### Keys

The `on_key(kv_i32 key)` handler receives:

* ordinary characters as ASCII/CP866 codes (`'a'`, `'5'`, `' '`);
* **Ctrl+letter** as codes 1…26 (Ctrl+A = 1, Ctrl+S = 19, Ctrl+L = 12);
* special keys as values above 255:

| Constant | Key |
|---|---|
| `KV_KEY_UP` | Arrow up |
| `KV_KEY_DOWN` | Arrow down |
| `KV_KEY_LEFT` | Arrow left |
| `KV_KEY_RIGHT` | Arrow right |
| `KV_KEY_ENTER` | Enter (this is `'\n'`) |
| `KV_KEY_BKSP` | Backspace (this is 8) |
| `KV_KEY_TAB` | Tab (this is 9) |

**Esc never reaches the application** — that key closes the window, so you can
always escape from a frozen program.

### Mouse

`on_click(x, y, button)` is called on a press inside the client area. The
coordinates are already relative to the window. `button` equals 1 for the left
button.

Tracking movement without a press is not available yet.

---

## Working with files

Files live on disk in the KvFS filesystem and **survive a reboot**.

| Function | Returns |
|---|---|
| `file_read(name, buf, max)` | The number of bytes read, or an error code |
| `file_write(name, buf, size)` | 0 on success |
| `file_delete(name)` | 0 on success |
| `file_list(index, name40, &size)` | 0 while files remain |
| `file_exists(name)` | 1 if the file is there |

KvFS limits: **64 files**, a name of up to **39 bytes**, and a file occupies a
contiguous chain of sectors.

The typical pattern is to read the settings at start-up and write them at close:

```c
static char buf[256];

static void on_open(void) {
    kv_i32 got = sys->file_read("myapp.cfg", buf, sizeof(buf) - 1);
    if (got > 0) { buf[got] = 0; /* parse it */ }
}

static void on_close(void) {
    sys->file_write("myapp.cfg", buf, sys->str_len(buf));
}
```

If there is no disk in the system these functions return an error — check the
result and never assume a write succeeded.

---

## Interface language

The system runs in English by default and can switch to Russian at run time, so
applications should follow it. Two things are provided by the ABI:

```c
kv_i32 lang(void);              /* 0 = English, 1 = Russian */
KV_T(sys, "Save", "Сохранить")  /* picks a string for the current language */
```

`KV_T` is a macro over `lang()`, so it costs one comparison:

```c
sys->text(20, 20, KV_T(sys, "Score", "Счёт"), fg, 0xFFFFFFFF);
```

The window title is a special case: `kv_app_t` must be `static`, and a static
initialiser cannot call a function. Assign a localised title inside `kapp_main`:

```c
kv_app_t *kapp_main(const kv_api_t *api) {
    sys = api;
    me.title = KV_T(api, "Clock", "Часы");
    return &me;
}
```

All three bundled samples do exactly this — use them as a template.

---

## Building

### What you need

`gcc` with 32-bit target support (`gcc-multilib`), `ld`, `python3`. The same
tools that build the OS itself.

### The normal way

Put your `.c` into `sdk/apps/` and run:

```bash
cd sdk
make
```

The Makefile builds **every** file in `apps/`, each into a separate application,
and lays the results out in `sdk/build/` and `release/apps/`.

The desktop name is taken from a comment in the first lines:

```c
/* KAPP_NAME "My program" */
```

### By hand

```bash
gcc -m32 -march=i586 -std=gnu11 -ffreestanding -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -nostdlib -nostdinc \
    -Wall -Wextra -O2 -I../include -isystem $(gcc -m32 -print-file-name=include) \
    -c apps/hello.c -o build/hello.o

ld -m elf_i386 -T kapp.ld -nostdlib -o build/hello.elf build/hello.o

python3 mkkapp.py build/hello.elf build/hello.kapp "Hello"
```

Every flag matters:

* `-ffreestanding -nostdlib -nostdinc` — there is no libc and there never will be;
* `-fno-pic -fno-pie` — the code is built for a fixed address;
* `-T kapp.ld` — placement at `0x00E00000` and the image boundary labels;
* `-m32 -march=i586` — 32 bits, no SSE.

The linker warning `LOAD segment with RWX permissions` is normal: we have no
per-page permission split.

---

## Installing into the system

Four ways. **The first is the main one**: it works everywhere, including VMware,
VirtualBox and real hardware, and needs no disk.

### 1. Embed the application into the boot image (recommended)

```bash
./sdk/addapp.sh my_program.kapp
```

The script puts the `.kapp` inside the ISO, writes it into the bootloader menu
and rebuilds the images. The program will be in the system **right after boot** —
no disk, no network, no USB stick required.

Then attach `release/kvantos.iso` to a virtual machine or burn it to a disc.
Inside the system: `guimenu` → key **G** → your program is in the list.

Here is how that works: GRUB loads the `.kapp` files into memory as Multiboot
modules and hands the kernel their list; the kernel puts the files into ramfs.
Programs can be launched straight from there, installing to disk is optional —
it is only needed so that an application can **save** its files.

The remaining ways are for cases where a disk already exists.

### 2. Straight into a disk image (for QEMU)

```bash
python3 sdk/mkdisk.py disk.img 64 sdk/build/clock.kapp sdk/build/notes.kapp
qemu-system-i386 -cdrom release/kvantos.iso -hda disk.img -m 256
```

The first run creates a 64 MiB image with a ready filesystem. The applications
will be in the list right after boot.

To add a program to an existing image:

```bash
python3 sdk/mkdisk.py --add disk.img sdk/build/hello.kapp
python3 sdk/mkdisk.py --list disk.img     # inspect the contents
```

### 3. From inside the system

If the disk has not been formatted yet, do it once:

```
kvant:/$ format
```

Or use the **Format disk** button in the Programs window. After that a file is
moved from ramfs onto the disk:

```
kvant:/$ install hello.kapp
kvant:/$ apps
```

### 4. On a real machine

Write the ISO to a disc or a USB stick, boot, run `format` (**all data on the
disk will be erased**), then install the applications.

### Shell commands for disk work

| Command | What it does |
|---|---|
| `disk` | List the ATA disks |
| `format` | Format the disk for KvFS |
| `df` | Disk usage |
| `dls` | Files on the disk |
| `dcat FILE` | Show a file from the disk |
| `install FILE` | Move a file from ramfs onto the disk |
| `uninstall FILE` | Delete it from the disk |
| `apps` | Installed applications |

---

## What you must not do

An application runs in the kernel ring, so the prohibitions rest on agreement,
not on hardware protection. Break one and the system will remove the program,
but it is better not to get there.

**You must not:**

* **touch memory outside your own image and the heap.** Everything you need is
  reachable through `kv_api_t`;
* **loop forever inside a handler.** The shell is single-threaded: until
  `on_draw` returns the system draws no frames and answers nothing. Split long
  work across frames;
* **call kernel functions by name.** Their addresses change from build to build.
  Only through the table;
* **count on libc.** There is no `printf`, no `malloc`, no `strcpy`. The ABI has
  equivalents;
* **use floating point.** The FPU is not initialised and `-march=i586` gives no
  SSE. Count in integers, as the Clock sample does with its integer sine table;
* **divide 64-bit numbers.** That pulls `__udivdi3` out of libgcc, which is
  absent under `-nostdlib`: you get a link error. Narrow to 32 bits;
* **keep state between runs in memory.** The image is overwritten on unload.
  Write to a file.

**You may:**

* keep as much static data as you like (it lands in bss);
* allocate memory through `alloc`;
* read and write files;
* play sounds, ask for the time, write to the log.

---

## Debugging

There is no hardware debugger for applications, so the main technique is logging:

```c
sys->log("got this far");

char buf[64];
sys->format(buf, sizeof(buf), "x=%d, len=%u", x, len);
sys->log(buf);
```

The lines are visible in the **Terminal** window of the graphical shell.

The second technique is drawing debug data straight into the window: variable
values, area boundaries, click coordinates. That is faster than guessing.

If an application crashes, the kernel shows the reason in the window: "memory
access at address 0x…", "Divide by zero" and the like. The reason is printed in
English because it comes from the CPU exception table.

Common causes of a crash:

| Symptom | The usual cause |
|---|---|
| A crash right at launch | `kapp_main` returned a non-static structure |
| "memory access at address 0x0" | A `NULL` was dereferenced |
| The link error `undefined reference` | A libc function was used |
| `__udivdi3` at build time | A 64-bit division |
| The window is empty | `on_draw` is unset or draws nothing |
| No keys arrive | The window is not active, or it was Esc |

---

## Error reference

### Application launch errors

| Message | Cause |
|---|---|
| "this is not a KvantOS application (no KAPP signature)" | The file never went through `mkkapp.py` |
| "the application targets ABI N, the system has ABI M" | Rebuild against the current `kvapp.h` |
| "invalid load address in the header" | Built without `kapp.ld` |
| "the entry point lies outside the application image" | A wrong linker script |
| "the application is larger than 2 MiB" | Shrink the image |
| "kapp_main() returned zero" | A missing `return &me` |
| "no disk attached" | There is no disk, or it is not formatted |

### Filesystem errors

| Code | Meaning |
|---|---|
| −1 | The filesystem is not mounted |
| −2 | A disk I/O error |
| −3 | File not found |
| −4 | Empty file name |
| −5 | The name is longer than 39 characters |
| −6 | The directory is full (64 files maximum) |
| −7 | Not enough contiguous space on the disk |

---

## Examples

Three ready applications with detailed comments are bundled.

### `sdk/apps/clock.c` — Clock

An analogue dial with hands and a digital time readout. It demonstrates:

* drawing with primitives (`line`, `fill`, `pixel`);
* trigonometry **without floating point** — an integer sine table multiplied
  by 1024;
* reading the real-time clock;
* handling keys and clicks;
* switching themes.

Keys: **S** the second hand, **D** the dark theme, **B** a beep.

### `sdk/apps/snake.c` — Snake

A game: the snake crawls around a grid, eats food and grows. It demonstrates
**time-based animation** — movement driven from `on_tick` with a step of
`hz() / 6`, independent of the frame rate.

Keys: **arrows** to turn, **space** to restart.

### `sdk/apps/notes.c` — Notes

A text editor keeping its note on disk. It demonstrates:

* typing text and a blinking cursor;
* wrapping lines to the window width;
* custom buttons and click hit-testing;
* **saving and reading a file** — the note survives a reboot.

Keys: **Ctrl+S** to save, **Ctrl+L** to re-read from disk.

---

## Appendix: how KvFS is arranged

You do not need this to write applications, but it helps if you want to work
with a disk image from outside.

```
   sector 0        superblock: the KVFS signature, version, counters
   sectors 1…8     directory: 64 entries of 64 bytes
   sectors 9…      data, in contiguous chains
```

A directory entry: name (40 bytes) + start sector + sector count + size + flags
(1 — in use, 2 — an executable file) + reserve.

A file occupies a contiguous run; deleting one leaves a hole, and a new file is
placed by the "first suitable gap" rule. There is no fragmentation — that keeps
both the kernel and the tools simple.

The entry size is checked at compile time (`_Static_assert`) so that the kernel
and `sdk/mkdisk.py` cannot drift apart in the layout.

---

## Appendix: the SDK files

| File | Purpose |
|---|---|
| `include/kvapp.h` | The only header an application needs |
| `sdk/kapp.ld` | Placement at address `0x00E00000` |
| `sdk/mkkapp.py` | Packing an ELF into a `.kapp` |
| `sdk/mkdisk.py` | Creating a disk image with KvFS |
| `sdk/Makefile` | Building all the samples |
| `sdk/apps/` | The application sources |
