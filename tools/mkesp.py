#!/usr/bin/env python3
# ============================================================
#  KvantOS 3.0 - EFI System Partition image builder
#
#  Pure Python, no mtools / mkfs.vfat needed: builds a FAT16
#  image with
#      /EFI/BOOT/BOOTX64.EFI   - the UEFI boot stub
#      /boot/kvant64.bin       - the 64-bit kernel
#      /boot/apps/*.kapp       - the applications
#
#  File names that do not fit into 8.3 (the .kapp extension is
#  four characters long) get proper VFAT long-name entries -
#  the UEFI FAT driver understands them on any firmware.
#
#  Usage:
#    python3 tools/mkesp.py out.img kvantefi.efi kvant64.bin apps_dir
# ============================================================
import os
import struct
import sys

SEC = 512
ROOT_ENTRIES = 512
FAT16_MIN_CLUSTERS = 4085
FAT16_EOC = 0xFFFF


def fits_83(name):
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    return len(base) <= 8 and len(ext) <= 3 and " " not in name


def short_checksum(short11):
    c = 0
    for b in short11.encode("ascii"):
        c = ((c & 1) << 7) + (c >> 1) + b
        c &= 0xFF
    return c


def lfn_entries(name, short11, seq_start=1):
    """VFAT long-name entries, lowest sequence number LAST in the list."""
    ucs = name.encode("utf-16-le") + b"\x00\x00"
    if len(ucs) % 26:
        pad = 26 - len(ucs) % 26
        if pad >= 2:
            ucs += b"\xff\xff"      # terminator
            pad -= 2
        ucs += b"\xff\xff" * (pad // 2)
    n = len(ucs) // 26
    chk = short_checksum(short11)
    out = []
    for i in range(n):
        part = ucs[i * 26:(i + 1) * 26]
        seq = seq_start + (n - 1 - i)
        if i == 0:
            seq |= 0x40
        e = bytearray(32)
        e[0] = seq
        e[1:11] = part[0:10]
        e[11] = 0x0F
        e[12] = 0
        e[13] = chk
        e[14:26] = part[10:22]
        # e[26:28] stays zero
        e[28:32] = part[22:26]
        out.append(bytes(e))
    out.reverse()
    return out


class Node(object):
    def __init__(self, name, is_dir):
        self.name = name
        self.is_dir = is_dir
        self.children = []          # for directories
        self.blob = b""             # for files


class Fat16(object):
    def __init__(self, total_sectors):
        self.spc = 1
        self.reserved = 1
        self.nfats = 2
        self.root_dir_sectors = ROOT_ENTRIES * 32 // SEC
        self.total = total_sectors

        self.fat_sectors = 16
        for _ in range(8):
            data = self.total - self.reserved - self.nfats * self.fat_sectors \
                   - self.root_dir_sectors
            clusters = data // self.spc
            fs = ((clusters + 2) * 2 + SEC - 1) // SEC
            if fs == self.fat_sectors:
                break
            self.fat_sectors = fs

        data = self.total - self.reserved - self.nfats * self.fat_sectors \
               - self.root_dir_sectors
        self.clusters = data // self.spc
        if not (FAT16_MIN_CLUSTERS <= self.clusters < 65525):
            raise ValueError("cluster count %d is not FAT16" % self.clusters)

        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        self.next_cluster = 2
        self.data = {}              # cluster -> SEC bytes

    def alloc(self, count):
        chain = []
        c = self.next_cluster
        while len(chain) < count:
            while c < len(self.fat) and self.fat[c] != 0:
                c += 1
            if c >= len(self.fat):
                raise ValueError("image too small")
            chain.append(c)
            c += 1
        for i, cl in enumerate(chain):
            self.fat[cl] = FAT16_EOC if i == len(chain) - 1 else chain[i + 1]
        self.next_cluster = chain[-1] + 1
        return chain

    def write_chain(self, chain, blob):
        full = bytearray(blob)
        if len(full) % SEC:
            full += b"\0" * (SEC - len(full) % SEC)
        need = len(chain) * self.spc * SEC
        if len(full) > need:
            raise ValueError("chain too short")
        full += b"\0" * (need - len(full))
        for i, cl in enumerate(chain):
            self.data[cl] = bytes(full[i * SEC:(i + 1) * SEC])

    def build(self, root, label="KVANTOSESP"):
        """Render a Node tree into the image and return the bytes."""
        self._short_used = set()
        root_bytes = self._render_dir(root, is_root=True)

        img = bytearray(self.total * SEC)
        bpb = bytearray(SEC)
        bpb[0:3] = b"\xEB\x3C\x90"
        bpb[3:11] = b"KVANTEFI"
        struct.pack_into("<H", bpb, 11, SEC)
        bpb[13] = self.spc
        struct.pack_into("<H", bpb, 14, self.reserved)
        bpb[16] = self.nfats
        struct.pack_into("<H", bpb, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bpb, 19, self.total)
        bpb[21] = 0xF8
        struct.pack_into("<H", bpb, 22, self.fat_sectors)
        struct.pack_into("<H", bpb, 24, 32)
        struct.pack_into("<H", bpb, 26, 8)
        struct.pack_into("<I", bpb, 36, 0x20260825)      # serial
        bpb[39:50] = label[:11].ljust(11).encode("ascii")
        bpb[54:62] = b"FAT16   "
        bpb[510:512] = b"\x55\xAA"
        img[0:SEC] = bpb

        fat_bytes = bytearray(self.fat_sectors * SEC)
        for i, v in enumerate(self.fat):
            struct.pack_into("<H", fat_bytes, i * 2, v & 0xFFFF)
        off = self.reserved * SEC
        for _ in range(self.nfats):
            img[off:off + len(fat_bytes)] = fat_bytes
            off += len(fat_bytes)

        img[off:off + len(root_bytes)] = root_bytes
        off += self.root_dir_sectors * SEC

        data_start = off
        for cl, blob in self.data.items():
            img[data_start + (cl - 2) * SEC:
                data_start + (cl - 1) * SEC] = blob
        return bytes(img)

    # ---- rendering ----
    def _unique_short(self, name):
        if fits_83(name):
            base, ext = (name.rsplit(".", 1) + [""])[:2] if "." in name \
                else (name, "")
            s = base.upper().ljust(8) + ext.upper().ljust(3)
            if s not in self._short_used:
                self._short_used.add(s)
                return s
        # generate NAME~n.EXT (extension trimmed to three characters)
        if "." in name:
            base, ext = name.rsplit(".", 1)
        else:
            base, ext = name, ""
        ext3 = ext[:3].upper()
        stem = "".join(ch for ch in base.upper() if ch.isalnum())[:6] or "FILE"
        for i in range(1, 100):
            s = f"{stem}~{i}".ljust(8)[:8] + ext3.ljust(3)
            if s not in self._short_used:
                self._short_used.add(s)
                return s
        raise ValueError("too many similar names")

    def _short_entry(self, short11, attr, cluster, size, case=0):
        e = bytearray(32)
        e[0:11] = short11.encode("ascii")
        e[11] = attr
        e[12] = case
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def _render_dir(self, node, is_root=False):
        entries = bytearray()

        if not is_root:
            entries += self._short_entry("." .ljust(8) + " " * 3, 0x10, 0, 0)
            entries += self._short_entry("..".ljust(8) + " " * 3, 0x10, 0, 0)

        for child in node.children:
            if child.is_dir:
                blob = self._render_dir(child)
                chain = self.alloc(max(1, (len(blob) + self.spc * SEC - 1)
                                          // (self.spc * SEC)))
                # patch the "." entry with the directory's own cluster
                blob = blob[:26] + struct.pack("<H", chain[0]) + blob[28:]
                self.write_chain(chain, blob)
                cluster, size, attr = chain[0], 0, 0x10
            else:
                chain = self.alloc(max(1, (len(child.blob) + self.spc * SEC - 1)
                                          // (self.spc * SEC)))
                self.write_chain(chain, child.blob)
                cluster, size, attr = chain[0], len(child.blob), 0x20

            if fits_83(child.name):
                base, ext = child.name.rsplit(".", 1) if "." in child.name \
                    else (child.name, "")
                case = (0x08 if base.islower() else 0) | \
                       (0x10 if ext and ext.islower() else 0)
                short11 = base.upper().ljust(8) + ext.upper().ljust(3)
                self._short_used.add(short11)
                entries += self._short_entry(short11, attr, cluster, size, case)
            else:
                short11 = self._unique_short(child.name)
                entries += b"".join(lfn_entries(child.name, short11))
                entries += self._short_entry(short11, attr, cluster, size)

        if is_root:
            # volume label
            lbl = "KVANTOSESP ".encode("ascii")
            e = bytearray(32)
            e[0:11] = lbl
            e[11] = 0x08
            entries += bytes(e)
            entries += b"\0" * (ROOT_ENTRIES * 32 - len(entries))
            return bytes(entries)

        n_clusters = max(1, (len(entries) + self.spc * SEC - 1)
                         // (self.spc * SEC))
        entries += b"\0" * (n_clusters * self.spc * SEC - len(entries))
        return bytes(entries)


def build_esp_image(out_path, efi_path, kernel_path, apps_dir):
    """Build the ESP image; also importable from other scripts."""
    root = Node("", True)
    efi_dir = Node("EFI", True)
    boot_dir = Node("BOOT", True)
    kboot = Node("boot", True)
    apps = Node("apps", True)
    root.children += [efi_dir, kboot]
    efi_dir.children.append(boot_dir)
    kboot.children.append(apps)

    boot_dir.children.append(Node("BOOTX64.EFI", False))
    boot_dir.children[-1].blob = open(efi_path, "rb").read()

    k = Node("kvant64.bin", False)
    k.blob = open(kernel_path, "rb").read()
    kboot.children.append(k)

    napps = 0
    if os.path.isdir(apps_dir):
        for name in sorted(os.listdir(apps_dir)):
            if name.endswith(".kapp"):
                a = Node(name, False)
                a.blob = open(os.path.join(apps_dir, name), "rb").read()
                apps.children.append(a)
                napps += 1
    if napps > 12:
        print("mkesp: at most 12 apps fit the apps directory")
        return 1

    fat = None
    sectors = 8192
    while fat is None and sectors <= 1 << 20:
        try:
            f = Fat16(sectors)
            fat = f
        except ValueError:
            sectors *= 2
    if fat is None:
        print("mkesp: cannot size the image")
        return 1

    img = fat.build(root)
    with open(out_path, "wb") as f:
        f.write(img)
    print(f"mkesp: {out_path}: {len(img)} B, {fat.clusters} clusters (FAT16), "
          f"{napps} apps")
    return 0


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    return build_esp_image(*sys.argv[1:5])


if __name__ == "__main__":
    sys.exit(main())
