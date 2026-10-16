# In-system C Workshop

Open **C Workshop** by single-clicking its desktop icon or using **KVANT → C Workshop**. The built-in editor
and compiler produce native i386 or x86-64 `.kapp` files (format 1, ABI 3) inside KvantOS.
No host GCC, network, assembler or interpreter is involved in that build.
This is a small, explicitly limited C implementation, **not full ISO C**.

The target follows the running kernel: BIOS/i386 builds 32-bit apps, UEFI/x86-64
builds 64-bit apps. Pointers have native width; `int` remains 32-bit on both.
Move source files between architectures and rebuild them; generated binaries
are architecture-specific and cannot run under the other kernel.

## Use

* **Build** compiles and saves a native `.kapp`; **Run** rebuilds and starts it.
  A failed build never runs stale output. Errors show a line number and move the cursor.
* **Open** opens a RAM/disk picker: choose a file, then click Open. Use the wheel
  or arrow buttons to scroll the file list. No typed path or Enter is required.
* **Save** writes the current file, asking for a name for new documents.
  **Save as** changes the name. Replacing another existing file needs confirmation.
* **New**, open, close and desktop exit protect unsaved text with Save / Don't save /
  Cancel. **Help** has a clickable Close help button. Use window crosses to close,
  the taskbar/menu to switch, and KVANT → Exit to console to leave the desktop.
* Click text to place the caret; drag to select; dragging beyond an edge auto-scrolls.
  Copy / Cut / Paste / Undo / Select all are clickable. Clipboard is shared with
  Notepad; document state and one-step undo/redo are independent. Click Undo again
  to redo. UTF-8 cursor/selection/deletion respects character boundaries.
* Both scrollbars are draggable; wheel scrolls vertically, Shift+wheel horizontally.
  Scrolling does not move or chase the caret.
* Keyboard remains for text and **optional** Ctrl+C/V/X/A/Z/S, Ctrl+O/N/W, F5/F6,
  F1 help, F2 open. Arrows, Home/End, Delete, Backspace, Enter, Tab and Shift+arrows
  edit text. Ctrl+U clears a filename; Enter accepts, Esc cancels. Esc in the text
  only clears selection. Letters B/G/E no longer launch desktop windows; games
  retain their keyboard controls.

Limits: 128 lines, 120 **bytes** per line. Overflow, binary/oversize loads and
failed line joins never silently truncate or discard the current document.

Use a flat filename with at most 18 characters before `.c`: `hello.c` becomes
`hello.kapp`. Without mounted **KvFS**, both files are in RAM and **lost on reboot**;
the status says so. With KvFS both are saved to RAM and disk. After reboot choose
**Open → Disk / KvFS**, select the source and click Open. The picker shows `.c`
files on the chosen medium; Program Manager shows `.kapp`. Direct save to FAT32/NTFS
or arbitrary VFS paths is not implemented. Notepad now also saves to mounted KvFS.

Disk persistence requires a supported **IDE/ATA PIO** disk, not AHCI/SATA
or NVMe. Under QEMU q35, attach a separate `piix3-ide` controller as shown
in RELEASE.md: q35 defaults `-drive if=ide` to SATA/AHCI, which this OS
does not detect.

## FAT32 USB source import (new development preview)

Click **Import**, **Refresh USB**, choose a folder and `.c`/`.C` file,
optionally edit its local name, then **Import**. The validated copy opens in
RAM; Save/Build/Run work normally. The flash drive is not modified. UTF-8 BOM
and CRLF are supported; invalid/oversized text leaves the old document intact.
**UHCI + high-speed EHCI**, no OHCI/xHCI/hubs; not in the published 3.1.2 ISO.
[Full instructions](../USB-IMPORT.md).

## Example

```c
int count = 0;
void draw(void) {
    clear(rgb(24, 32, 50));
    text(20, 20, "My C application", rgb(230, 240, 255), -1);
    fill(20 + count * 12, 90, 40, 40, rgb(60, 160, 240));
}
void click(int x, int y, int button) {
    count = (count + 1) % 20;
}
```

The compiler supplies `kapp_main`, the application descriptor, a 480x300
client area and the file header. The source filename becomes the window title.
Define `draw()` or `main()`; optional callbacks are `open()`, `key(int k)`,
`click(int x,int y,int button)`, `tick()` and `close()`. A zero-argument `main`
is used as `open` if there is no `open`. `close` runs on loader unload.

Call API functions directly, without `#include` or a `sys->` prefix.
Names and argument order match `kv_api_t` in `include/kvapp.h`:

* `width height rgb clear pixel fill rect line text text_width status`
* `alloc release mem_set mem_copy str_len str_cmp str_copy format`
* `ticks hz seconds delta_ms clock beep random log lang`
* `file_read file_write file_delete file_list file_exists` (KvFS)

For example `text(x,y,"UTF-8",foreground,-1)` uses a transparent background.
Use `alloc(64)` to obtain a string buffer for `format(buffer,64,"%d",value)`,
and `release(buffer)` to return it. `lang()` returns 0 for English, 1 for Russian.

## Language boundary

Supported: 32-bit signed `int`, signed `char`, `void`, pointers, globals with
literal/zero initialization, locals, named function parameters and prototypes,
helper functions and recursion, blocks, `if/else`, `while`, `for`, `return`,
`break`, `continue`, arithmetic/comparison/bitwise operators, short-circuit
`&& ||`, assignment and `+= -= *= /= %=`, prefix/postfix increments, address-of,
dereference, pointer indexing/arithmetic, strings/characters, comments and
basic escapes. `const` is accepted but its write restrictions are not checked.

Not supported: preprocessor, headers, `typedef`, structs/unions/enums,
`static/extern`, unsigned/long types, floating point, array declarations,
casts, `sizeof`, `switch`, `do/while`, ternary/comma operators, function pointers,
external libraries, object files or full C type checking. Use `0` rather than
a `NULL` macro. Existing SDK examples with `sys->...` still require the
external GCC SDK; they do not compile unchanged in the workshop.

Limits: 16000-byte source; editor 128 lines of 120 bytes; 24000-byte code and
8000-byte data sections; output fits 32 KiB. Up to 8 arguments, 256 symbols
(including API), 128 local records and 512 (i386) / 1024 (x86-64) bytes of local stack per function,
8 nested loops, 24 parser nesting levels and 511 bytes per string literal.

## Safety and tests

Like other KvantOS applications, generated native code runs in **ring 0**,
not a sandbox. Invalid pointers, deep recursion and infinite loops can crash
or hang the OS. Save source first and experiment in QEMU with a disk copy.
Parser safety does not imply runtime program safety.

```sh
python3 -m pip install unicorn
make -C tests/mcc check           # 73 execution checks per target (i386 and x86-64)
make -C tests/mcc sanitize ramfs  # 40000 mutations per target, atomic RAM save
python3 tests/mcc/smoke.py        # guest builds/runs, error handling, reboot, diskless
```

The smoke test runs both BIOS/i386 and UEFI/x86-64 guests and only writes its
own `tests/mcc/build/{bios32,uefi64}/workshop.img` disks. It also checks native
locals, double pointers, heap allocations, eight-argument functions and
variadic API calls inside the OS. The 64-bit Unicorn tests place the stack
above 4 GiB and verify SysV alignment and callee-saved registers.
See [the Russian guide](WORKSHOP.ru.md) for the same workflow and examples.
