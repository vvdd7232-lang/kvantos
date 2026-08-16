#!/usr/bin/env python3
# ============================================================
#  KvantOS SDK - application packer
#
#  Turns a linked ELF into a .kapp file: cuts out the ready memory
#  image and prepends the 64-byte header the kernel loader reads.
#
#  Usage:
#     python3 mkkapp.py input.elf output.kapp "Title"
# ============================================================
import struct
import subprocess
import sys

LOAD_BASE      = 0x00E00000     # must match KAPP_LOAD_BASE
API_VERSION    = 1
FORMAT_VERSION = 1
HEADER_SIZE    = 64
MAX_SIZE       = 0x00200000     # 2 MiB
FLAG_WINDOW    = 0x0001


def fail(msg):
    print(f"  ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def run(*args):
    """Call a binutils tool with a readable diagnostic."""
    try:
        return subprocess.run(args, capture_output=True, text=True, check=True).stdout
    except FileNotFoundError:
        fail(f"program {args[0]} not found (install binutils)")
    except subprocess.CalledProcessError as e:
        fail(f"{args[0]} failed:\n{e.stderr}")


def symbol_address(elf, name):
    """Address of a symbol from the ELF table."""
    for line in run("nm", elf).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print("Usage: mkkapp.py input.elf output.kapp [\"Title\"]")
        sys.exit(1)

    elf, out = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else out.rsplit("/", 1)[-1].replace(".kapp", "")

    # --- entry point ---
    entry = symbol_address(elf, "kapp_main")
    if entry is None:
        fail("the application has no kapp_main - every .kapp must define it")

    # --- image boundaries ---
    file_end  = symbol_address(elf, "__kapp_file_end")
    bss_start = symbol_address(elf, "__bss_start")
    bss_end   = symbol_address(elf, "__bss_end")
    if file_end is None or bss_start is None or bss_end is None:
        fail("layout labels not found - build with sdk/kapp.ld")

    # --- cut out the ready memory image ---
    run("objcopy", "-O", "binary",
        "--set-section-flags", ".bss=alloc,load,contents",
        elf, out + ".tmp")
    with open(out + ".tmp", "rb") as f:
        blob = f.read()

    code_size = file_end - LOAD_BASE
    if code_size <= 0:
        fail("empty image: the application contains no code")
    if len(blob) < code_size:
        fail(f"image shorter than expected ({len(blob)} < {code_size})")
    blob = blob[:code_size]

    bss_size = bss_end - bss_start
    if code_size + bss_size > MAX_SIZE:
        fail(f"application larger than 2 MiB ({(code_size + bss_size) // 1024} KiB)")

    if not (LOAD_BASE <= entry < LOAD_BASE + code_size):
        fail(f"entry point 0x{entry:08X} lies outside the image - wrong linker script?")

    # --- header ---
    nm = name.encode("utf-8")[:31]
    header = struct.pack(
        "<4sHHIIIIII32s",
        b"KAPP",
        FORMAT_VERSION,
        HEADER_SIZE,
        API_VERSION,
        FLAG_WINDOW,
        LOAD_BASE,
        entry,
        code_size,
        bss_size,
        nm,
    )
    assert len(header) == HEADER_SIZE, f"header is {len(header)} bytes instead of {HEADER_SIZE}"

    with open(out, "wb") as f:
        f.write(header + blob)

    import os
    os.remove(out + ".tmp")

    total = len(header) + len(blob)
    print(f"  KAPP {out}")
    print(f"       title      : {name}")
    print(f"       entry point: 0x{entry:08X}")
    print(f"       code+data  : {code_size} bytes")
    print(f"       bss        : {bss_size} bytes")
    print(f"       file       : {total} bytes ({(total + 1023) // 1024} KiB)")


if __name__ == "__main__":
    main()
