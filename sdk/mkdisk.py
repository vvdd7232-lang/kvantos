#!/usr/bin/env python3
# ============================================================
#  KvantOS SDK - preparing a disk with the KvFS filesystem
#
#  Creates a disk image and writes applications into it, so that the
#  system sees them right after boot - no manual work in the shell.
#
#  Examples:
#     python3 mkdisk.py disk.img 64                  - empty 64 MiB disk
#     python3 mkdisk.py disk.img 64 build/clock.kapp - with a program
#     python3 mkdisk.py --list disk.img              - show the contents
#
#  Layout (must match kernel/kvfs.c):
#     sector 0     - superblock
#     sectors 1-8  - directory, 64 entries of 64 bytes
#     sectors 9+   - data
# ============================================================
import os
import struct
import sys

SECTOR       = 512
MAGIC        = 0x5346564B          # "KVFS"
VERSION      = 2
# The first megabyte of the disk belongs to the bootloader (MBR + the
# GRUB body), so the filesystem starts at sector 2048.
KVFS_BASE    = 2048
DIR_SECTOR   = KVFS_BASE + 1
DIR_SECTORS  = 8
DATA_START   = DIR_SECTOR + DIR_SECTORS
MAX_FILES    = 64
ENTRY_SIZE   = 64
NAME_LEN     = 40

FLAG_USED = 1
FLAG_EXEC = 2


def fail(msg):
    print(f"  ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


class KvFS:
    """A disk image holding the KvFS filesystem."""

    def __init__(self, path, size_mb=None):
        self.path = path
        if size_mb is not None:
            self.total = (size_mb * 1024 * 1024) // SECTOR
            if self.total < DATA_START + 64:
                fail("the disk is smaller than the minimum size")
            with open(path, "wb") as f:
                f.truncate(self.total * SECTOR)
            self.entries = [None] * MAX_FILES
            self._flush()
        else:
            if not os.path.exists(path):
                fail(f"file {path} not found")
            self.total = os.path.getsize(path) // SECTOR
            self._load()

    # ---------- reading an existing image ----------
    def _load(self):
        with open(self.path, "rb") as f:
            f.seek(KVFS_BASE * SECTOR)
            sb = f.read(SECTOR)
            magic, ver, total, data_start, count = struct.unpack("<IIIII", sb[:20])
            if magic != MAGIC:
                fail("the image carries no KvFS filesystem (create it first)")
            if ver != VERSION:
                fail(f"filesystem version {ver}, this tool understands {VERSION}")
            self.total = total
            f.seek(DIR_SECTOR * SECTOR)
            raw = f.read(DIR_SECTORS * SECTOR)

        self.entries = []
        for i in range(MAX_FILES):
            chunk = raw[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
            name = chunk[:NAME_LEN].split(b"\0")[0].decode("utf-8", "replace")
            start, sectors, size, flags, _, _ = struct.unpack("<IIIIII", chunk[NAME_LEN:])
            self.entries.append(
                {"name": name, "start": start, "sectors": sectors,
                 "size": size, "flags": flags} if flags & FLAG_USED else None)

    # ---------- writing the service areas ----------
    def _flush(self):
        used = sum(1 for e in self.entries if e)
        sb = struct.pack("<IIIII", MAGIC, VERSION, self.total, DATA_START, used)
        sb += b"\0" * (SECTOR - len(sb))

        raw = bytearray()
        for e in self.entries:
            if e is None:
                raw += b"\0" * ENTRY_SIZE
                continue
            nm = e["name"].encode("utf-8")[:NAME_LEN - 1]
            nm += b"\0" * (NAME_LEN - len(nm))
            raw += nm + struct.pack("<IIIIII", e["start"], e["sectors"],
                                    e["size"], e["flags"], 0, 0)

        with open(self.path, "r+b") as f:
            f.seek(KVFS_BASE * SECTOR)
            f.write(sb)
            f.seek(DIR_SECTOR * SECTOR)
            f.write(bytes(raw))

    # ---------- looking for a contiguous run ----------
    def _find_space(self, need):
        candidate = DATA_START
        for _ in range(MAX_FILES + 1):
            clash = False
            nxt = 0
            for e in self.entries:
                if not e:
                    continue
                a, b = e["start"], e["start"] + e["sectors"]
                if candidate < b and a < candidate + need:
                    clash = True
                    nxt = max(nxt, b)
            if not clash:
                if candidate + need > self.total:
                    fail("no contiguous free space left on the disk")
                return candidate
            candidate = nxt
        fail("could not place the file")

    # ---------- adding a file ----------
    def add(self, name, data, is_exec):
        if len(name.encode("utf-8")) >= NAME_LEN:
            fail(f"the name '{name}' is longer than {NAME_LEN - 1} bytes")

        # an existing file with the same name is removed
        for i, e in enumerate(self.entries):
            if e and e["name"] == name:
                self.entries[i] = None

        need = max(1, (len(data) + SECTOR - 1) // SECTOR)
        slot = next((i for i, e in enumerate(self.entries) if e is None), None)
        if slot is None:
            fail("the directory is full (64 files maximum)")

        start = self._find_space(need)
        self.entries[slot] = {
            "name": name, "start": start, "sectors": need,
            "size": len(data), "flags": FLAG_USED | (FLAG_EXEC if is_exec else 0),
        }

        padded = data + b"\0" * (need * SECTOR - len(data))
        with open(self.path, "r+b") as f:
            f.seek(start * SECTOR)
            f.write(padded)
        self._flush()

    def listing(self):
        return [e for e in self.entries if e]


def do_list(path):
    fs = KvFS(path)
    files = fs.listing()
    print(f"\n  Image: {path}")
    print(f"  Size: {fs.total * SECTOR // (1024 * 1024)} MiB, "
          f"files: {len(files)} of {MAX_FILES}\n")
    if not files:
        print("  (empty)\n")
        return
    print("      SIZE  TYPE   SECTORS      NAME")
    for e in files:
        kind = "prog." if e["flags"] & FLAG_EXEC else "file "
        print(f"   {e['size']:7}  {kind}  {e['start']:5}+{e['sectors']:-4} "
              f"  {e['name']}")
    print()


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return

    if args[0] == "--list":
        if len(args) < 2:
            fail("specify an image: mkdisk.py --list disk.img")
        do_list(args[1])
        return

    # adding to an existing image
    if args[0] == "--add":
        if len(args) < 3:
            fail("usage: mkdisk.py --add disk.img file...")
        fs = KvFS(args[1])
        for src in args[2:]:
            add_file(fs, src)
        do_list(args[1])
        return

    # creating a new image
    path = args[0]
    size_mb = int(args[1]) if len(args) > 1 else 64
    files = args[2:]

    fs = KvFS(path, size_mb)
    print(f"\n  Created image {path} ({size_mb} MiB) with a KvFS filesystem")
    for src in files:
        add_file(fs, src)
    do_list(path)
    print("  Run:  qemu-system-i386 -cdrom release/kvantos.iso "
          f"-hda {path} -m 256\n")


def add_file(fs, src):
    if not os.path.exists(src):
        fail(f"file {src} not found")
    with open(src, "rb") as f:
        data = f.read()
    name = os.path.basename(src)
    is_exec = name.endswith(".kapp") or data[:4] == b"KAPP"
    fs.add(name, data, is_exec)
    print(f"    + {name}  ({len(data)} bytes{', application' if is_exec else ''})")


if __name__ == "__main__":
    main()
