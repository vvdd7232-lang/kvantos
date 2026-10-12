#!/usr/bin/env python3
# ============================================================
#  KvantOS - GRUB-free ISO builder
#
#  Assembles the direct boot stub (boot/direct.asm), lays out the
#  El Torito image (stub + read table + kernel segments + .kapp
#  modules) and packs everything into an ISO9660 image with
#  pycdlib (vendored under tools/vendor).
#
#  Usage:
#    python3 tools/mkdirect.py build/kvant.bin release/apps \
#        build/kvantos-direct.iso [--cmdline "kvant.bin"]
# ============================================================
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")

STUB_PAD = 2048          # stub = first four sectors (loaded by the BIOS)
TABLE_OFF = 1536         # the read table lives in the fourth sector
COPY_OFF = 1792          # ... followed by the bounce copy table
BOUNCE_BASE = 0x10000    # INT 13h reads land here (see direct.asm)
MOD_BASE = 0x200000      # .kapp modules land at 2 MiB
MOD_ALIGN = 4096         # Multiboot bit 0: modules page aligned
MAX_APPS = 16


def elf_loads(data: bytes):
    """Return (entry, [(vaddr, bytes)], bss_start, bss_end)."""
    if data[:4] != b"\x7fELF":
        # raw binary at 1 MiB, no extra BSS
        return 0x100000, [(0x100000, data)], 0x100000 + len(data), \
            0x100000 + len(data)
    e_entry, = struct.unpack_from("<I", data, 0x18)
    e_phoff, = struct.unpack_from("<I", data, 0x1C)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
    loads, bss_start, bss_end = [], 0, 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = \
            struct.unpack_from("<IIIIII", data, off)
        if p_type != 1:          # PT_LOAD
            continue
        seg = data[p_offset:p_offset + p_filesz]
        loads.append((p_vaddr, seg))
        bss_start = max(bss_start, p_vaddr + p_filesz)
        bss_end = max(bss_end, p_vaddr + p_memsz)
    return e_entry, loads, bss_start, bss_end


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    kernel_path, apps_dir, iso_path = sys.argv[1], sys.argv[2], sys.argv[3]
    cmdline = "kvant.bin"
    if "--cmdline" in sys.argv:
        cmdline = sys.argv[sys.argv.index("--cmdline") + 1]

    kernel = open(kernel_path, "rb").read()
    entry, loads, bss_start, bss_end = elf_loads(kernel)

    apps = []
    if os.path.isdir(apps_dir):
        for name in sorted(os.listdir(apps_dir)):
            if name.endswith(".kapp"):
                apps.append((name, open(os.path.join(apps_dir, name), "rb").read()))
    if len(apps) > MAX_APPS:
        print("mkdirect: at most %d modules are supported" % MAX_APPS)
        return 1

    # ---- lay out the image: stub, table, then 4K-aligned blobs ----
    blobs = [(vaddr, seg) for vaddr, seg in loads]
    mod_dst = MOD_BASE
    app_meta = []
    for name, data in apps:
        blobs.append((mod_dst, data))
        app_meta.append((name, data, mod_dst))
        mod_dst = (mod_dst + len(data) + MOD_ALIGN - 1) & ~(MOD_ALIGN - 1)

    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "direct-apps.inc"), "w") as f:
        for name, data, dst in app_meta:
            f.write(f"    dd 0x{dst:08x}, {len(data)}    ; {name}\n")
    names_blob = b""
    with open(os.path.join(BUILD, "direct-nameoffs.inc"), "w") as f, \
            open(os.path.join(BUILD, "direct-names.inc"), "w") as g:
        for name, _, _ in app_meta:
            f.write(f"    dd {len(names_blob)}    ; {name}\n")
            g.write(f"    db '{name}', 0\n")
            names_blob += name.encode() + b"\0"

    defines = [
        f"BSS_START=0x{bss_start:x}",
        f"BSS_END=0x{bss_end:x}",
        f"ENTRY_POINT=0x{entry:x}",
        f"N_APPS={len(apps)}",
        f"CMDLINE_LEN={len(cmdline) + 1}",
        f"CMDLINE='{cmdline}'",
    ]
    stub_bin = os.path.join(BUILD, "direct_stub.bin")
    cmd = ["nasm", "-f", "bin", "-w-no-number-overflow", "-o", stub_bin,
           "-I", BUILD, os.path.join(ROOT, "boot", "direct.asm")]
    for d in defines:
        cmd += ["-D", d]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print("mkdirect: nasm failed")
        return r.returncode
    stub = open(stub_bin, "rb").read()
    if len(stub) > TABLE_OFF:
        print(f"mkdirect: the stub grew to {len(stub)} bytes, max {TABLE_OFF}")
        return 1
    stub += b"\0" * (STUB_PAD - len(stub))

    # ---- append the blobs and build the read + copy tables ----
    img = bytearray(stub)
    entries, copies = [], []
    bounce = BOUNCE_BASE
    for dst, data in blobs:
        off = (len(img) + MOD_ALIGN - 1) & ~(MOD_ALIGN - 1)
        img += b"\0" * (off - len(img))
        padded = (len(data) + MOD_ALIGN - 1) & ~(MOD_ALIGN - 1)
        entries.append((off // 512, padded // 512, bounce))
        copies.append((bounce, dst, len(data)))
        bounce += padded
        img += data
        if padded > len(data):
            img += b"\0" * (padded - len(data))
    if bounce > 0x78000:
        print(f"mkdirect: bounce area grew to {bounce:#x}, out of room")
        return 1

    table = struct.pack("<I", len(entries))
    for lba, sectors, b_dst in entries:
        table += struct.pack("<III", lba, sectors, b_dst)
    ctable = struct.pack("<I", len(copies))
    for src, dst, size in copies:
        ctable += struct.pack("<III", src, dst, size)
    if TABLE_OFF + len(table) > COPY_OFF or COPY_OFF + len(ctable) > STUB_PAD:
        print("mkdirect: table overflow")
        return 1
    img[TABLE_OFF:TABLE_OFF + len(table)] = table
    img[COPY_OFF:COPY_OFF + len(ctable)] = ctable

    img = bytes(img)
    img_path = os.path.join(BUILD, "direct.img")
    open(img_path, "wb").write(img)
    print(f"mkdirect: kernel {len(kernel)} B -> {len(loads)} segments, "
          f"BSS up to 0x{bss_end:x}, {len(apps)} apps, "
          f"image {len(img)} B, entry 0x{entry:x}")

    # ---- the ISO with El Torito ----
    sys.path.insert(0, os.path.join(HERE, "vendor"))
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.new(vol_ident="KVANTOS")
    iso.add_directory("/BOOT")
    iso.add_file(img_path, "/BOOT/DIRECT.IMG;1")
    iso.add_eltorito("/BOOT/DIRECT.IMG;1", boot_load_size=STUB_PAD // 512,
                     platform_id=0, boot_info_table=False, media_name="noemul")

    # ---- UEFI path (KvantOS 3.0): ESP image as the second El Torito entry ----
    efi_path = kernel64_path = None
    if "--efi" in sys.argv:
        efi_path = sys.argv[sys.argv.index("--efi") + 1]
    if "--kernel64" in sys.argv:
        kernel64_path = sys.argv[sys.argv.index("--kernel64") + 1]
    if (efi_path and kernel64_path and os.path.exists(efi_path)
            and os.path.exists(kernel64_path)):
        sys.path.insert(0, HERE)
        import mkesp
        # the ESP carries the 64-BIT applications: a .kapp built for the
        # 64-bit kernel refuses to run on the 32-bit one and vice versa
        apps64_dir = apps_dir
        if "--apps64" in sys.argv:
            apps64_dir = sys.argv[sys.argv.index("--apps64") + 1]
        esp_path = os.path.join(BUILD, "esp.img")
        mkesp.build_esp_image(esp_path, efi_path, kernel64_path, apps64_dir)
        iso.add_file(esp_path, "/ESP.IMG;1")
        iso.add_directory("/EFI")
        iso.add_directory("/EFI/BOOT")
        iso.add_file(efi_path, "/EFI/BOOT/BOOTX64.EFI;1")
        iso.add_file(kernel64_path, "/BOOT/KVANT64.BIN;1")
        iso.add_eltorito("/ESP.IMG;1", platform_id=0xEF, efi=True)
        print("mkdirect: UEFI entry added (esp.img + BOOTX64.EFI)")
    else:
        print("mkdirect: no UEFI files given (--efi/--kernel64), BIOS-only ISO")

    iso.write(iso_path)
    iso.close()
    print(f"mkdirect: {iso_path} written ({os.path.getsize(iso_path)} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
