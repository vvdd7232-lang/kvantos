# KvantOS code audit — the bugs found and the fixes

*Read this in [Русский](AUDIT.ru.md).*

Date: 16 August 2026. Method: building every module with hardened warnings,
manual review of the risky places, arithmetic checked in python3, regression
runs in QEMU.

With `-Wshadow -Wcast-align -Wnull-dereference -Wduplicated-cond -Wlogical-op`
the compiler produced no warnings at all except three harmless
`-Wmissing-prototypes` (`kmain`, `isr_handler`, `irq_handler` — they are called
from assembly and need no prototype). Every defect listed below was found by
reasoning about the logic, not by the compiler.

| # | Module | Defect | Consequence | Fix |
|---|--------|--------|-------------|-----|
| 1 | vbe.c | `vbe_vram_bytes()` was called every frame, and inside it `pci_bar_size()` writes `0xFFFFFFFF` into the video card BAR | **Critical on real hardware:** video memory decoding is dropped for an instant — flicker or a hang | Measured once in `vbe_init()`, then cached in `vram_cached` |
| 2 | heap.c | `kcalloc(n, size)` did not check `n * size` for overflow | `kcalloc(0x10000, 0x10000)` → 0 bytes instead of a refusal, then heap corruption | A `n > 0xFFFFFFFF / size` check |
| 3 | ramfs.c / shell.c | `ramfs_create()` silently truncated a name to 23 characters | `ramfs_find()` could not find the full name → a silent duplicate file | Codes −4 (empty name) and −5 (too long) plus messages in kvsh |
| 4 | gui.c | `(mx - gx) / 8` — division of negatives rounds towards zero | A click 1–7 pixels left of the canvas painted cell 0 | A sign check `dx >= 0 && dy >= 0` before dividing |
| 5 | gui.c | `fb_set_target(backbuf)` with no NULL check when rolling a mode back | On low memory — a write through a null pointer | A comment plus reliance on correct NULL handling |
| 6 | gui.c | Icons hard-wired into a single column with a 90 px step | At 640×480 the lower icons slid under the taskbar | A column layout derived from the screen height |
| 7 | vga.c | `vga_panic_screen()` did not recompute the character grid | After a resolution change the panic text used the old `cols`/`rows` | `cols`/`rows` are recomputed before clearing |
| 8 | timer.c | `timer_init(0)` → a division by zero, and the divisor might not fit into 16 bits | A triple fault at start-up | A 19…10000 Hz limit plus a clamped divisor |
| 9 | timer.c | `beep()` divided by zero at exotic frequencies | An exception instead of a sound | Frequencies outside 19…20000 Hz are rejected |
| 10 | heap.c | `ALIGN8(size)` overflowed for sizes near 2³² | A huge request turned into a tiny block → **heap corruption** | Refusal when `size > 0xFFFFFFF0` and when size exceeds the heap |
| 11 | shell.c | `alloc N` was not checked against the heap size | A pointless walk over the whole block list | A check against `heap_stats()` and a clear refusal |
| 12 | sched.c | `task_create()` dereferenced `current` without checking | A crash when called before `sched_init()` | NULL checks on `entry` and `current` |
| 13 | keyboard.c | `shift ^ caps` was applied to **every** key | With CapsLock the digit `1` produced `!`, and Shift+1 produced `1` | CapsLock now affects only the letters `a`–`z` |
| 14 | keyboard.c | `kbd_getchar()` spun on `task_yield()` before the scheduler started | 100% CPU load in the wait loop | `hlt` while interrupts are enabled |

## Verifying the fixes in QEMU

- **#13** — with CapsLock on, typing produced `ECHO 12345`: the letters went
  upper case, the digits stayed digits (before the fix it was `!@#$%`).
- **#3** — `write averyveryverylongfilenamehere` → "file name too long
  (23 characters maximum)"; a short name is created normally.
- **#11** — `alloc 999999999` → "more requested than the heap size (10240 KiB)",
  with no panic; `alloc 4096` works.
- **#6** — the GUI at 640×480: the icons moved into a second column
  (`screenshots/25_gui_640x480.png`).
- Switching 640×480 ↔ 1024×768, entering and leaving the GUI, `mem` — no
  panics; not a single exception in the COM1 log.

## The build after the fixes

```
text 104571   data 233   bss 236224   dec 341028 (0x53424)
release/kvantos.iso  960 512 B  md5 486da90f42645c09cd7d797bd1b14ea7
```

---

# The boot problem on an old laptop ("Welcome to GRUB!" and a black screen)

## What was happening

The symptom was reproduced in QEMU on a machine without PCI
(`-machine isapc -cpu pentium`): GRUB printed its greeting, showed the menu,
executed `multiboot` with return code 0 — and the machine went into an
**endless reboot loop**, without emitting a single byte on COM1. In other words
the kernel died within the very first cycles, before the serial port was even
initialised.

## The cause (the main one)

By default `gcc -m32` builds for **i686** and inserts instructions that old
processors do not have. Inside the kernel we found:

- `cmov`/`cmovcc` — 60 of them (introduced in the Pentium Pro, 1995);
- `bswap`, `cpuid` — absent on the i386/i486.

On a CPU older than the Pentium Pro the very first `cmov` raises a #UD (invalid
instruction). At that point there is no exception handler yet → a triple fault →
an instant reboot. Exactly what you saw.

## The fixes

| # | File | What was done |
|---|------|---------------|
| 15 | Makefile | `-march=i586 -mtune=generic` — all 60 `cmov` instructions vanished from the kernel |
| 16 | cpu.c | `cpuid` is only executed if the CPU supports it (checking the ID bit 21 in EFLAGS); otherwise "i386/i486" |
| 17 | grub.cfg | The menu is forced into **text** mode (`terminal_output console`) — on old video cards switching to gfxterm produced a black screen |
| 18 | grub.cfg | The menu entries were rewritten in **Latin script**: text VGA has no Cyrillic, so Russian titles turned into `?????` |
| 19 | grub.cfg | The first entry uses `gfxpayload=keep` — no mode is imposed, whatever the firmware gave is kept |
| 20 | grub.cfg | GRUB output is mirrored to COM1 (38400) — the log can be taken off a broken machine |
| 21 | grub.cfg + main.c | A new **safe mode** entry: text mode, PCI/VBE/mouse skipped |

## Verification

| Configuration | Before | After |
|---------------|--------|-------|
| `isapc`, Pentium, no PCI | endless reboot, a 0-byte log | **boots all the way to `kvant:/$`** |
| `pc`, Pentium, `-vga std` | — | boots, the Russian text is correct |
| `pc`, Pentium II, `-vga std` | ok | ok, no regressions |
| `pc`, Pentium III, `-vga cirrus` | ok | ok, a 24-bit framebuffer |
| safe mode | — | "PCI, VBE and the mouse were skipped", the shell works |

## What to do on your laptop

1. Rewrite the disc/USB stick with the new `release/kvantos.iso`.
2. If the default is a black screen again — pick
   **"KvantOS - VGA text 80x25 (old machines)"** from the menu.
3. If that does not help either — **"safe mode"**.
4. If you need a log: attach a null modem to COM1 at 38400 8N1 — both GRUB and
   the kernel now write there.

---

# A refinement: the Samsung RV410 (NP-RV410L)

The model is known exactly, and that **changes the diagnosis**.

## What is withdrawn

The Pentium Dual-Core **T4500** is a Penryn core (2010), essentially a Core 2.
It supports `cmov` without any reservations. So **the i686 instruction
hypothesis does not apply to your machine** — fixes #15/#16 remain a useful
safety net for genuinely ancient hardware, but they were not the cause of the
black screen on an RV410.

## What was actually found

Running `videoinfo` in GRUB showed that **there is no 1366×768 mode in the VBE
tables at all** — neither in QEMU nor, as a rule, on a GMA 4500M. The width is
not a multiple of 8 and BIOSes usually do not publish such modes. The native
resolution of your screen is unreachable through VBE.

Much more important is the mode selection mechanism. Established
experimentally: **for a Multiboot kernel GRUB takes the video mode from the
kernel header, and `gfxpayload` in grub.cfg has no effect on it.** This was
verified directly — with `gfxpayload=1024x768x32` and zeroes in the header the
result was 800×600×24, i.e. the config was ignored.

The practical conclusion for the RV410: if the GMA 4500M could not deliver the
requested 1024×768, GRUB handed over no framebuffer, and before the fixes the
kernel did not survive that.

## The resulting fixes

| # | File | What was done |
|---|------|---------------|
| 22 | vbe.c | The 1366×768 mode was added to the table — reachable via `vidmode` through BGA if the card can manage it |
| 23 | boot.asm | A comment recording the established fact: the mode is set here, `gfxpayload` does not work for multiboot |
| 24 | grub.cfg | A "widescreen 1280x768" entry was added — a genuinely existing 16:9 mode in place of the missing 1366×768 |

There was an intermediate mistake too: the width/height in the header were
zeroed and the resolution fell to 800×600 on every configuration. 1024×768 was
restored.

## The final verification

| Configuration | Result |
|---|---|
| `-vga std -cpu core2duo -m 1024` | a 1024×768×32 framebuffer, the shell is OK |
| `-vga cirrus -cpu core2duo` | a 1024×768×24 framebuffer, the shell is OK |
| `-vga std -cpu pentium` | a 1024×768×32 framebuffer, the shell is OK |
| `isapc -cpu pentium` (no PCI) | the shell is OK, no panics |
| 1536 MB of RAM | 393 184 pages, no panics |

## What to do on an RV410

1. Write the new `release/kvantos.iso`. The image is **hybrid** (verified: MBR
   code + El Torito), so both a blank disc and a USB stick written with `dd` or
   Rufus in DD mode will do.
2. In the BIOS disable "Fast BIOS Mode" if it is there; boot in Legacy/CSM, the
   image is deliberately **without UEFI** (normal for 2010).
3. If the black screen returns, try in order:
   **"VGA text 80x25"** → **"safe mode"**. Text mode on an RV410 has to work.
4. The laptop has a **VGA** port: attach an external monitor — sometimes the
   built-in panel stays silent while the external output shows the picture.

---

# The real cause of the black screen was found

Continuing the investigation with the model in mind, we found a defect that
explains your symptom completely — and it was not in GRUB but in the kernel.

## The failure mechanism

1. The kernel header asks for 1024×768×32 → GRUB **switches the card into
   graphics**.
2. The GMA 4500M hands back a mode, but in a form the kernel does not accept
   (a palette `type`, a non-standard bpp and so on).
3. `fb_init()` returns 0.
4. `have_fb = 0` → `vga_init()` takes the **text** branch and writes to
   `0xB8000`.
5. Meanwhile the card is **in graphics mode**, where `0xB8000` is not mapped.

The result: **the kernel is fully alive and running, but the screen is black.**
Exactly what you observed. We had recorded this trap earlier in another context,
but the "framebuffer rejected" path did not take it into account.

## Fix #25

In `main.c`: if GRUB supplied framebuffer information but `fb_init()` did not
accept it, the kernel **returns the card to text mode itself** through
`vbe_force_text()` and writes a diagnostic to COM1.

## The proof

`fb_init()` was temporarily forced to always return 0 — precisely reproducing
the behaviour of an incompatible video card:

- the frame became **720×400** (the card returned to text);
- the screen showed a full shell with Russian text
  (`screenshots/29_fallback_text.png`);
- before the fix this would have been a black screen.

The simulation was removed and normal graphics re-verified: q35/std, pc/cirrus,
pc/pentium — 1024×768 everywhere, the shell starts.

## Fix #26: the `hwreport` command

A `hwreport` command was added (`hw` for short): CPU, RAM size, PCI device
count, video card model with its PCI ID, the current video mode, the VBE
interface and whether a mouse is present. The output is mirrored to COM1. An
example is in `screenshots/28_hwreport.png`.

If something goes wrong on the laptop, this command shows in a single screen
exactly what the kernel saw on your hardware.

---

# Diagnosis without a COM port (an important correction)

My earlier advice to "take the log off COM1" **does not suit your laptop**: by
your own list of connectors the RV410 has USB, VGA, RJ-45, audio and a card
reader — there is no physical COM port. COM1 output stays useful in virtual
machines, but on an RV410 it is unavailable.

So diagnostics that work **without a screen and without serial** were added.

| # | File | What was done |
|---|------|---------------|
| 27 | keyboard.c | **A hidden bug was found:** the IRQ handler treated controller replies (0xFA ACK, 0xFE resend) as scan codes. The very first LED command completely broke keyboard input |
| 28 | keyboard.c | `kbd_set_leds()` — indicator control; the command exchange is indivisible (`irq_save`) and the ACK is read explicitly |
| 29 | main.c | Stage signalling: NumLock lights up after keyboard initialisation; on reaching the shell all three indicators flash for 150 ms with an 880 Hz beep |
| 30 | keyboard.c | The CapsLock indicator is synchronised with the real state |
| 31 | shell.c | The command name is lower-cased: with CapsLock on, `MEM` was not found. Arguments (file names) keep their case |

## How to read this on the laptop

- **NumLock lit up** — the kernel is alive and reached keyboard initialisation.
- **All three indicators blinked plus a beep** — the kernel is fully booted and
  the shell is running. If the screen is black at that point, the problem is
  only in the picture output; the system works.
- **Nothing blinked** — the kernel never started; try the "VGA text 80x25"
  entry, then "safe mode".

## Verification

- Bug #27 was reproduced and eliminated: before the fix, after the LEDs were
  set the command `help` could not be typed at all; afterwards it runs.
- 11 CapsLock toggles in a row straight inside the IRQ handler — input stays
  alive, no panics.
- CapsLock is still correct: letters change case, the digits `12345` do not
  turn into `!@#$%`.
- `MEM` runs with CapsLock on; the file `Prov` keeps its capital letter and
  `cat Prov` returns `Data`.

One false alarm during testing deserves a note: the file was created as `ROV`
instead of `Prov`. The cause was in the test itself — the QEMU monitor does not
accept `sendkey P`, capitals must be sent as `shift-p`. The kernel was working
correctly.

---

# Another hang at "Welcome to GRUB!" — the cause was found

None of the earlier fixes helped, because they all concerned the kernel and the
menu while the machine stalled **earlier** — at the stage where GRUB reads its
own files off the drive.

## Two real causes

**1. The `serial` command in grub.cfg — my own miscalculation.**
I added it myself "for diagnostics". On a machine without a physical UART,
writing to port 0x3F8 makes GRUB wait for a timeout **on every character**.
Output effectively stops right after "Welcome to GRUB!". The RV410 has no COM
port. The command was removed.

**2. GRUB was reading 276 modules and a 2.4 MB font off the DVD.**
`grub-mkrescue` leaves the modules as separate files on the disc. Every
`insmod`, the menu rendering and the kernel load are new accesses to the drive.
On a worn 2010 DVD drive the first bad track hangs the boot solidly, with
exactly the picture you described.

## The solution: a monolithic image

| # | What was done |
|---|---------------|
| 32 | The `serial` commands were removed from grub.cfg |
| 33 | A move from `grub-mkrescue` to `grub-mkstandalone`: every module and grub.cfg are sewn into core.img |
| 34 | **The kernel was embedded inside the boot image (memdisk)** — `/boot/kvant.bin` is read from RAM, not from the drive |
| 35 | A new `make floppy` target: a 1.44 MB floppy image, bypassing the DVD entirely |

The ISO shrank from **10 475 520 to 940 032 bytes** (10.5 MB → 918 KB):
everything that used to be read off the disc left the image.

## Side defects found while building the floppy

- `dd` of the boot sector overwrote the **BPB** (bytes 3–89) → "non DOS media".
  Then it turned out that core.img from sector 2 overwrites the FAT itself. The
  final solution: the floppy carries **no filesystem at all** and the kernel
  lives inside core.img.
- In a monolithic image the default root is the memdisk. An intermediate variant
  with `search --file` worked, but became unnecessary once the kernel was
  embedded, and was removed.

## Verification

| Medium | Configuration | Result |
|---|---|---|
| ISO | q35/core2duo/std | 1024×768×32, OK |
| ISO | pc/core2duo/cirrus | 1024×768×24, OK |
| ISO | pc/pentium/std | 1024×768×32, OK |
| ISO | isapc without PCI | text mode, OK |
| Floppy | pc/core2duo | OK |
| Floppy | pc/pentium | OK |

Zero panics across all six runs.

## What to do on an RV410

1. **Write the new ISO** (918 KB) — onto a blank disc at the lowest speed, or
   onto a USB stick (the image is hybrid).
2. If it hangs at "Welcome to GRUB!" again, the drive is at fault. Then use
   **`release/kvantos-floppy.img`**: it reads a single sector and works from
   memory afterwards. It can be written to USB with Rufus/dd — the RV410 BIOS
   can do USB-FDD emulation.
3. Watch the indicators: NumLock lit → the kernel started.

---

# The keyboard does not respond on the real laptop

The floppy booted, so the bootloader and the kernel are fine. Your screenshot
shows the main thing: the status bar counts time `00:00:22` and says "3 tasks".
**The system has not frozen** — the timer ticks, the scheduler runs. It is the
keyboard that does not answer.

## Two causes

**1. The i8042 controller was never initialised.**
`keyboard_init()` consisted of two lines: clear the buffer and hook IRQ1. None
of the mandatory work was done:

- the keyboard port was not enabled with command `0xAE` (the BIOS could have
  left it disabled);
- bit 0 of the configuration byte — **the IRQ1 enable** — was not set;
- translation into scan code set 1 (bit 6) was not enabled, although the
  `map_lower`/`map_upper` tables are written for exactly that set;
- command `0xF4` — "start sending scan codes" — was not issued;
- the junk byte left over by the BIOS was not drained. While it sits in the
  output buffer **there will be no new IRQ1 at all**.

QEMU hands over the controller already enabled and translating, which is why the
defect never surfaced in any earlier test. A real i8042 does not behave that way.

**2. `mouse_init()` clobbered the keyboard bits.**
The controller's configuration byte is shared by both ports. The mouse read it,
changed its own bits and wrote it back — without reaffirming the keyboard bits.
If the BIOS had left them cleared, input disappeared entirely.

## The fixes

| # | File | What was done |
|---|------|---------------|
| 36 | keyboard.c | Full i8042 initialisation: buffer drain, `0xAE`, configuration bits 0/4/6, command `0xF4`, another drain |
| 37 | mouse.c | The mouse explicitly reaffirms the keyboard bits (0x01, ~0x10, 0x40) |
| 38 | keyboard.c + timer.c | `kbd_poll()` — a fallback poll on the timer tick in case IRQ1 never arrives |

## Verification

The fallback was tested harshly: **IRQ1 was masked out completely in the PIC**,
simulating a laptop where the interrupt never arrives. The result — `help` and
`mem` can still be typed and executed. The simulation was removed and the code
returned to its original state (verified: no leftovers).

| Medium | Configuration | Result |
|---|---|---|
| Floppy | pc/core2duo | OK, 0 panics |
| Floppy | pc/pentium | OK, 0 panics |
| ISO | q35/core2duo | OK, 0 panics |
| ISO | isapc without PCI | OK, 0 panics |

Input was checked separately: `help`, `mem`, `hwreport` — every command accepted.

## If the keyboard still stays silent

In the RV410 BIOS look for an item like **USB Legacy Support / USB Keyboard
Support** and try both positions. With emulation on, the BIOS sometimes grabs
the controller for itself.

---

# Graphics optimisation (10-15 fps on real hardware)

## Where the time was actually lost

Measurement revealed four bottlenecks, the first being the main one.

**1. Video memory was working in an uncached (UC) mode.**
MTRRs were never configured. On real hardware the PCI/PCIe region is uncacheable
by default: **every** pixel write goes out as a separate bus transaction and the
CPU waits for it to complete. Copying a 1024x768x32 frame (3 MiB) in that mode
takes tens of milliseconds. In QEMU this is invisible — there all the "video
memory" is ordinary host RAM, which is why the defect never surfaced in any
earlier run.

**2. All 3 MiB went into video memory every frame**, even when only the cursor
had moved.

**3. `fb_glyph` drew text pixel by pixel** — a bounds check and an address
recomputation for every pixel.

**4. `task_sleep(30)` was deaf.** On a slow machine it added 30 ms to an already
long frame; on a fast one it capped things at 33 fps.

## What was done

| # | File | Optimisation |
|---|------|--------------|
| 39 | mtrr.c (new) | **Write-combining** mode for the framebuffer through MTRRs: the CPU accumulates writes and sends them in 64-byte bursts. The main win on live hardware |
| 40 | fb.c | `fb_present` — the loop unrolled by 8 words (32 bytes per iteration) |
| 41 | fb.c | `fb_present_rows()` — flushing only the given rows |
| 42 | gui.c | A full frame flush only on input events; otherwise only the cursor strip and the panel reach video memory — tens of KiB instead of 3 MiB |
| 43 | gui.c | Once every 8 frames the whole frame is flushed, so that the live Monitor counters and the clock do not freeze |
| 44 | fb.c | A fast path in `fb_glyph` for 32 bits: no bounds checks, empty glyph rows skipped |
| 45 | string.c | `memcpy`/`memset` work in 4-byte words instead of a byte-by-byte loop |
| 46 | gui.c | An adaptive delay: sleep only for what remains of 30 ms, otherwise `task_yield()` right away |
| 47 | gui.c | **An fps counter on the panel** — so we measure rather than guess |

## Verification

`memcpy`/`memset` were compared against reference implementations across **1600
combinations** of offsets and lengths — no discrepancies. That is critical: a
bug in them would corrupt the whole system.

| Medium | Configuration | Result |
|---|---|---|
| Floppy | pc/core2duo | OK, 0 panics |
| Floppy | pc/pentium | OK, 0 panics |
| ISO | q35/core2duo | OK, 0 panics |
| ISO | isapc without PCI | OK, 0 panics |

In QEMU the counter shows 33 fps — that is the ceiling of the adaptive delay,
not a speed limit. The system monitor and the clock update correctly.

## What to expect on an RV410

The main effect comes from the MTRRs (#39): uncached video memory is exactly
what creates those 10-15 fps. I cannot honestly predict the size of the gain in
advance — it depends on whether the laptop BIOS enabled write-combining over the
GMA region by itself. **The exact figure will be shown by the fps counter in the
right-hand corner of the panel.**

If the gain turns out to be small, the next step is to lower the resolution
(`vidmode 800 600` halves the frame size) or move to 16 bits per pixel.

---

# Lifting the 33-frame ceiling

The ceiling was created by **two** causes, not one.

1. **A hard 30 ms frame budget** in the main GUI loop.
2. **Timer granularity.** The PIT ran at 100 Hz, i.e. a 10 ms step. Even without
   the budget, timing a frame more precisely than 33 fps was impossible, and
   `task_sleep(1)` would have slept the full 10 ms.

## What was done

| # | File | Change |
|---|------|--------|
| 48 | main.c | The PIT was moved to **1000 Hz** (a 1 ms step). The IRQ overhead is negligible, and we gained millisecond precision |
| 49 | gui.c | The frame rate limit was removed: `gui_fps_limit = 0` — we draw as fast as the hardware allows, merely yielding the CPU to the scheduler |
| 50 | gui.c | The **L** key cycles the limit: unlimited → 60 → 30 fps (useful for saving battery on a laptop) |
| 51 | gui.c | Hard-wired tick counts were replaced by `timer_hz()`: the double click (was 90), the settings message (was 400), the fps measurement window. Otherwise changing the PIT frequency would have broken them |
| 52 | gui.c | 64-bit divisions were removed — they pulled in `__udivdi3`, which is absent without libgcc |

## A side bug found: doubled characters

After the move to 1000 Hz, input started doubling characters: `gui` turned into
`ggui`. The cause was a race between the timer poll `kbd_poll()` and the IRQ1
handler: the timer managed to take the scan code out of port 0x60 first, and
`kbd_cb` then read an **already empty port** and got the same value again. At
100 Hz the race window was ten times narrower, so the defect never showed.

Fixed by two changes:

| # | File | Change |
|---|------|--------|
| 53 | keyboard.c | `kbd_cb` reads 0x60 only when the OBF status bit confirms a byte is there |
| 54 | keyboard.c | The polling loop in `kbd_poll` runs indivisibly (`irq_save`) |

## Verification

| Mode | Counter reading |
|---|---|
| Unlimited | **125-127 fps** |
| Limit 60 | 61 fps |
| Limit 30 | 30 fps |

It was 33 fps — it is now 125+. Cycling with the L key works round the loop, and
the character doubling is gone (verified: `gui` types correctly).

| Medium | Configuration | Result |
|---|---|---|
| Floppy | pc/core2duo, pc/pentium | OK, 0 panics, the timer at 1000 Hz |
| ISO | q35/core2duo | OK, 0 panics |
| ISO | isapc without PCI | OK, 0 panics |

## The editor: polish after the first test

A full text editor (the Editor application, key **E**) was added and tested on
the live system. The first run exposed three defects.

| # | File | Fix |
|---|---|---|
| 55 | gui.c | **Ctrl+S saved without asking for a name.** The combination called `ed_save()` directly, so the document silently went out under the default name and the name typed by the user was ignored. Ctrl+S now enters name-entry mode with the current name filled in — a "Save as" |
| 56 | gui.c | **The Editor icon had no picture** — only a caption hung on the desktop: `draw_icon_glyph()` did not know `APP_EDIT`. A sheet with lines and a pencil was drawn |
| 57 | gui.c | **The bottom row of icons ran into the taskbar.** The `per_col` formula ignored the 50 px top margin and the 58 px caption height. Verified for 1024x768, 800x600 and 640x480 — the captions fit everywhere |

### Verification

The QEMU scenario: desktop → **E** → type three lines → **Ctrl+S** → enter the
name `zametka.txt` → Enter → back to the shell.

```
kvant:/$ ls
     SIZE  NAME
      207  readme.txt
      152  license.txt
      201  poem.txt
      124  motd.txt
       43  zametka.txt

kvant:/$ cat zametka.txt
Privet KvantOS
Vtoraya stroka
Tretya stroka
```

The file was written under the requested name, the contents match character by
character, and the name appears in the file list on the right immediately. A
screenshot: `screenshots/32_editor.png`.

### The editor's features

Typing, arrows, Enter, Backspace, Tab (4 spaces), line numbering, current-line
highlighting, a blinking cursor, a scrollbar, a list of ramfs files on the right
(click to open) and a status line "Line N of M, column K". Buttons: New, Open,
Save, Delete. Ctrl+N for a new document, Ctrl+S to save, Esc to close the
window. Limit: 128 lines of 120 characters.

## Applications: disk, filesystem and ABI

The system learned to install programs. That required four new subsystems, hence
the size of this section.

| # | File | What was done |
|---|---|---|
| 58 | kernel/ata.c | An **ATA PIO, LBA28** disk driver. Polling instead of interrupts, timeouts on every wait: without them a missing controller would hang the boot. ATAPI drives and the "floating bus" (status 0xFF) are filtered out |
| 59 | kernel/kvfs.c | **KvFS**: a superblock, a 64-file directory, contiguous sector chains. Placement by "the first suitable gap" |
| 60 | include/kvapp.h | **The application ABI**: a table of 30 system functions, the window structure, key codes. The only contract between the kernel and a program |
| 61 | kernel/kapp.c | The `.kapp` loader: signature and ABI version checks, unpacking the image, calling the entry point, clipping drawing to the client area |
| 62 | kernel/gui.c | The Programs window (install, launch, remove) and the Application window, the icons, the **G** hotkey |
| 63 | kernel/shell.c | The commands `disk`, `format`, `df`, `dls`, `dcat`, `install`, `uninstall`, `apps` |

### Defects found and fixed

| # | Symptom | Cause and solution |
|---|---|---|
| 64 | `apps` printed `%-24s 1468720 KiB` | `kprintf` **cannot do left alignment**. The padding is now added with spaces by hand and the length is counted in UTF-8 characters, not bytes |
| 65 | The `mkdisk.py` tool could not read its own image | The directory entry in C came out **60 bytes instead of 64**. It was aligned and a `_Static_assert(sizeof(kvfs_dirent_t) == 64)` added — the compiler now catches any drift |
| 66 | **A crashing application took the whole OS down** | Two errors at once. First: `guard_set` was a function and saved the `esp` of its own frame — by the time of the return that frame was already overwritten and the jump landed in garbage (`eip=0x00000007`). It became the `GUARD_SET` macro, expanded inside the calling function whose frame stays alive for the whole run of the application |
| 67 | A write to address zero passed silently | **Page 0 was mapped present+rw**, so `*(int*)0 = 42` raised no exception and corrupted memory. The zero page is now left unmapped — dereferencing `NULL` gives an honest page fault |
| 68 | A panic instead of removing the program | `page_fault` and `isr_handler` now check `kapp_in_app()` and call `kapp_recover()`. No message is printed on screen: it would corrupt the graphics mode |

### Verification on the live system

| What was checked | Result |
|---|---|
| Disk detection | `QEMU HARDDISK, 131072 sectors, 64 MiB` |
| Launching an application from disk | The clock runs, the hands and the digital time are right |
| Keys and mouse in an application | **D** switches the theme, **S** the seconds, clicks are counted |
| An application writing a file | Notes saves its text to disk |
| **Persistence across a reboot** | A file written in the previous session was read back after a full power cycle |
| **An application crash** | The program was removed, the system stayed alive: 126 fps, the shell responds |
| A machine without a disk | Works, and honestly reports that installation is unavailable |

### A regression across four configurations

| Configuration | Disk | Result |
|---|---|---|
| pc / core2duo / ISO | yes | 0 panics |
| pc / pentium / floppy | no | 0 panics |
| q35 / core2duo / ISO | yes | 0 panics |
| isapc / pentium / ISO | no | 0 panics |

Screenshots: `screenshots/33_app_clock.png`, `34_app_notes_persist.png`,
`35_store.png`, `36_app_crash_survived.png`. The working debug frames are in
`testshots/app_*`.

## Delivering applications without a disk

The user reported: the `.kapp` application sits in Windows, the machine runs in
VMware, and there is no way to get the file inside. A check confirmed the gap:
`install` takes a file from ramfs, but there was nothing that could put a binary
file there, and no disk was attached to that VM.

| # | File | What was done |
|---|---|---|
| 69 | kernel/main.c | The kernel reads **Multiboot modules**: GRUB loads the `.kapp` files into memory and the kernel puts them into ramfs. The name comes from the module string with the path stripped |
| 70 | Makefile, grub/grub.cfg | Applications are embedded **inside the boot image** next to the kernel; `module` lines were added to every menu entry |
| 71 | kernel/kapp.c | The loader looks for an application on the disk first and **then in ramfs** — programs from the ISO run without being installed |
| 72 | kernel/gui.c | The Programs window shows applications from the image when no disk is found or it is unformatted (`W_ST_RAM0`) |
| 73 | sdk/addapp.sh | An "embed my .kapp into the ISO" script: it checks the KAPP signature, updates grub.cfg and rebuilds the images |
| 74 | sdk/apps/snake.c | A third sample — a game with time-based animation (`on_tick`, a step of `hz()/6`) |

### Mistakes along the way

| Symptom | Cause |
|---|---|
| `APP_GRAFT` was empty and no applications were embedded | The Makefile took the files from `sdk/build`, and a directory named `build` **is not kept in the snapshot**. The source was switched to `release/apps` |
| The first run showed an empty `ls` | The test used the old ISO: `make iso` does not rebuild the image when the target is considered fresh. `rm -f build/kvantos.iso` is required |
| The Programs window showed no list without a disk | The `!ata_present()` branch returned from the function before the listing call |

### Verification

| What | Result |
|---|---|
| Booting **with no disk at all** | `ls` shows `clock.kapp` and `notes.kapp` from the image |
| Launching from the image | The clock runs, the window and the status line are in place |
| `addapp.sh` with a user file | Snake was embedded, appears in the list and plays with the arrows |
| Module diagnostics | `flags=0x126d mods_count=2` — GRUB passes the modules correctly |

Screenshots: `screenshots/37_app_snake.png`, `38_apps_from_iso.png`.
The user-facing instructions are in `INSTALL.ru.md`.

## Checksums (current)

| File | Size, B | md5 |
|---|---|---|
| release/kvantos.iso | 1 005 568 | `330bbe1dac69a50c4a90c894f6d8cfc7` |
| release/kvantos-floppy.img | 1 474 560 | `df61defc58c68a7a2d4d0a319b140518` |
| release/kvant.bin | 166 916 | `52ee2b909f4e7f28bd2a6d24db0d0fff` |
