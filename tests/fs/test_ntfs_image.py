#!/usr/bin/env python3
"""Qualify the real read-only NTFS adapter on private regular-file images.

Requires mkntfs, ntfscp, ntfscat, ntfsinfo, ntfscluster, ntfsfallocate and ntfstruncate
from ntfs-3g. The script creates images only inside tests/fs/build/ntfs-image-*
temporary directories and never mounts or opens a physical block device.
"""
from __future__ import annotations
import hashlib
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FS_BUILD_PATH = ROOT / "tests/fs/build"
FS_BUILD = FS_BUILD_PATH.resolve()
FS_PARENT = (ROOT / "tests/fs").resolve()
HARNESS = FS_BUILD / "harness"
PAYLOAD = b"KvantOS NTFS private image qualification\n4KiB logical sector adapter\n"
LARGE_NAME = "Длинный файл.bin"
LARGE_BYTES = 1024 * 1024
IMAGE_BYTES = 128 * 1024 * 1024


def tool(env_name: str, fallback: str) -> str:
    value = os.environ.get(env_name)
    found = value or shutil.which(fallback)
    if not found or not Path(found).is_file():
        raise SystemExit(f"BLOCKED: missing {fallback}; set {env_name} or install ntfs-3g")
    return str(Path(found).resolve())


def run(args: list[str], *, env: dict[str, str], capture: bool = False) -> subprocess.CompletedProcess:
    print("+", " ".join(args), flush=True)
    return subprocess.run(args, check=True, env=env, stdout=subprocess.PIPE if capture else None)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def mbr_partition_image(volume: Path, target: Path, start_lba: int = 2048) -> None:
    sectors = IMAGE_BYTES // 512
    disk_sectors = start_lba + sectors
    fd = os.open(target, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    with os.fdopen(fd, "r+b") as out:
        out.truncate(disk_sectors * 512)
        mbr = bytearray(512)
        struct.pack_into("<B3sB3sII", mbr, 0x1BE, 0, b"\0\0\0", 0x07,
                         b"\0\0\0", start_lba, sectors)
        mbr[510:512] = b"\x55\xaa"
        out.seek(0)
        out.write(mbr)
        out.seek(start_lba * 512)
        with volume.open("rb") as src:
            shutil.copyfileobj(src, out, 1024 * 1024)
    if target.is_symlink() or not target.is_file() or target.stat().st_size != disk_sectors * 512:
        raise SystemExit("MBR wrapper is not the expected private regular file")


def main() -> None:
    if FS_BUILD_PATH.is_symlink() or FS_BUILD.parent != FS_PARENT:
        raise SystemExit("refusing a symlink or redirected filesystem-test build directory")
    if not HARNESS.is_file():
        raise SystemExit("BLOCKED: build tests/fs/build/harness first")
    mkntfs = tool("MKNTFS", "mkntfs")
    ntfscp = tool("NTFSCP", "ntfscp")
    ntfscat = tool("NTFSCAT", "ntfscat")
    ntfsinfo = tool("NTFSINFO", "ntfsinfo")
    ntfscluster = tool("NTFSCLUSTER", "ntfscluster")
    ntfsfallocate = tool("NTFSFALLOCATE", "ntfsfallocate")
    ntfstruncate = tool("NTFSTRUNCATE", "ntfstruncate")
    env = os.environ.copy()
    bin_dirs = {str(Path(p).resolve().parent) for p in
                (mkntfs, ntfscp, ntfscat, ntfsinfo, ntfscluster, ntfsfallocate, ntfstruncate)}
    env["PATH"] = os.pathsep.join(sorted(bin_dirs)) + os.pathsep + env.get("PATH", "")
    lib_dir = os.environ.get("NTFS_LIB_DIR")
    if lib_dir:
        env["LD_LIBRARY_PATH"] = str(Path(lib_dir).resolve()) + os.pathsep + env.get("LD_LIBRARY_PATH", "")

    with tempfile.TemporaryDirectory(prefix="ntfs-image-", dir=FS_BUILD) as tmp:
        work = Path(tmp).resolve()
        if work.parent != FS_BUILD or work.is_symlink():
            raise SystemExit("refusing image path outside private regular-file test directory")
        source = work / "proof.txt"
        source.write_bytes(PAYLOAD)
        large_source = work / "large.bin"
        with large_source.open("wb") as dst:
            for start in range(0, LARGE_BYTES, 64 * 1024):
                block = bytes(((start + i) * 131 + 17) % 251 for i in range(min(64 * 1024, LARGE_BYTES - start)))
                dst.write(block)
        large_payload = large_source.read_bytes()
        for bps in (512, 1024, 2048, 4096):
            image = work / f"ntfs-{bps}.img"
            # O_EXCL plus a fresh private temporary directory avoids following
            # a pre-existing path or truncating any caller-supplied file.
            fd = os.open(image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            with os.fdopen(fd, "r+b") as out:
                out.truncate(IMAGE_BYTES)
            if not image.is_file() or image.is_symlink() or image.stat().st_size != IMAGE_BYTES:
                raise SystemExit("image is not the expected private regular file")

            run([mkntfs, "--quick", "--force", "--sector-size", str(bps),
                 "--label", f"KV{bps}", str(image)], env=env)
            with image.open("rb") as src:
                boot = src.read(512)
            actual_bps = int.from_bytes(boot[0x0B:0x0D], "little")
            total_logical = int.from_bytes(boot[0x28:0x2C], "little")
            if boot[510:512] != b"\x55\xaa" or actual_bps != bps:
                raise SystemExit(f"mkntfs fixture geometry mismatch: requested={bps}, actual={actual_bps}")
            if not total_logical or total_logical * bps > IMAGE_BYTES:
                raise SystemExit("mkntfs volume length escapes the private image")

            run([ntfscp, str(image), str(source), "/proof.txt"], env=env)
            run([ntfscp, str(image), str(large_source), f"/{LARGE_NAME}"], env=env)
            independent = run([ntfscat, str(image), "/proof.txt"], env=env, capture=True)
            if independent.stdout != PAYLOAD:
                raise SystemExit(f"ntfscat mismatch for {bps}-byte-sector proof fixture")
            independent = run([ntfscat, str(image), f"/{LARGE_NAME}"], env=env, capture=True)
            if independent.stdout != large_payload:
                raise SystemExit(f"ntfscat mismatch for nonresident Unicode file at {bps} bytes/sector")
            info = run([ntfsinfo, "-F", f"/{LARGE_NAME}", str(image)], env=env, capture=True).stdout.decode(errors="replace")
            data_attr = info.split("Dumping attribute $DATA", 1)[-1]
            if not re.search(r"Resident:\s+No", data_attr):
                raise SystemExit("ntfsinfo did not confirm the large file is nonresident")

            before = digest(image)
            for expected_file, expected_name in ((source, "proof.txt"), (large_source, LARGE_NAME)):
                native = subprocess.run([str(HARNESS), str(image), "--ntfs-image", str(expected_file),
                                         "0", expected_name], env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
                print(native.stdout.decode(errors="replace"), end="", flush=True)
                if native.returncode:
                    raise SystemExit(f"KvantOS actual NTFS adapter failed for {bps}-byte sectors / {expected_name}: exit {native.returncode}")
            after = digest(image)
            if after != before:
                raise SystemExit(f"read-only driver changed {bps}-byte-sector fixture")
            print(f"PASS actual ntfs-3g {bps}-byte logical-sector image; resident+nonresident byte-exact reads, Unicode root listing, zero writes; SHA256 unchanged {after}", flush=True)

            if bps in (512, 4096):
                partitioned = work / f"ntfs-{bps}-mbr.img"
                mbr_partition_image(image, partitioned)
                partition_before = digest(partitioned)
                for expected_file, expected_name in ((source, "proof.txt"), (large_source, LARGE_NAME)):
                    native = subprocess.run([str(HARNESS), str(partitioned), "--ntfs-image",
                                             str(expected_file), "2048", expected_name],
                                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    print(native.stdout.decode(errors="replace"), end="", flush=True)
                    if native.returncode:
                        raise SystemExit(f"offset MBR NTFS image failed for {bps}-byte sectors / {expected_name}")
                if digest(partitioned) != partition_before:
                    raise SystemExit(f"read-only driver changed offset MBR image for {bps}-byte sectors")
                print(f"PASS NTFS MBR partition offset 2048 (512-byte LBA units), bps={bps}; whole-disk SHA unchanged", flush=True)

                malformed = work / f"ntfs-{bps}-mbr-oob.img"
                mbr_partition_image(image, malformed)
                with malformed.open("r+b") as out:
                    out.seek(0x1BE + 12)
                    out.write(struct.pack("<I", (IMAGE_BYTES // 512) + 1))
                malformed_before = digest(malformed)
                native = subprocess.run([str(HARNESS), str(malformed), "--ntfs-refuse"],
                                        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                print(native.stdout.decode(errors="replace"), end="", flush=True)
                if native.returncode or digest(malformed) != malformed_before:
                    raise SystemExit(f"out-of-bounds MBR NTFS extent was not safely refused for {bps}-byte sectors")
                print(f"PASS oversized MBR partition rejected without out-of-bounds reads, bps={bps}", flush=True)

        # A nonzero high word in NTFS total-sectors exceeds the driver's
        # 32-bit block addressing. Refuse instead of mounting a low-word alias.
        high_count_image = work / "ntfs-high-total-sectors.img"
        fd = os.open(high_count_image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        with os.fdopen(fd, "wb") as dst, (work / "ntfs-512.img").open("rb") as src:
            shutil.copyfileobj(src, dst, 1024 * 1024)
        with high_count_image.open("r+b") as out:
            out.seek(0x2C)
            out.write(struct.pack("<I", 1))
        high_count_before = digest(high_count_image)
        native = subprocess.run([str(HARNESS), str(high_count_image), "--ntfs-refuse"],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(native.stdout.decode(errors="replace"), end="", flush=True)
        if native.returncode or digest(high_count_image) != high_count_before:
            raise SystemExit("NTFS volume with nonzero high total-sectors word was not safely refused")
        print("PASS NTFS 64-bit total-sectors high word refused; no alias/OOB reads/writes", flush=True)

        # The VFS size ABI is 32-bit. A 4-GiB sparse NTFS stream must not
        # appear as an empty file or permit a low-DWORD alias.
        oversized_image = work / "ntfs-oversized-file.img"
        fd = os.open(oversized_image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        with os.fdopen(fd, "r+b") as dst:
            dst.truncate(IMAGE_BYTES)
        run([mkntfs, "--quick", "--force", "--sector-size", "512",
             "--label", "KVBIG", str(oversized_image)], env=env)
        oversized_source = work / "oversized-source.bin"
        oversized_source.write_bytes(b"nonzero-prefix-before-4GiB\n")
        run([ntfscp, str(oversized_image), str(oversized_source), "/oversized.bin"], env=env)
        inode_info = run([ntfsinfo, "-F", "/oversized.bin", str(oversized_image)],
                         env=env, capture=True).stdout.decode(errors="replace")
        inode_match = re.search(r"Dumping Inode\s+(\d+)", inode_info)
        if not inode_match:
            raise SystemExit("ntfsinfo did not identify the oversized-file MFT record")
        run([ntfstruncate, str(oversized_image), inode_match.group(1), str(1 << 32)], env=env)
        oversized_info = run([ntfsinfo, "-F", "/oversized.bin", str(oversized_image)],
                             env=env, capture=True).stdout.decode(errors="replace")
        data_attr = oversized_info.split("Dumping attribute $DATA", 1)[-1]
        if not re.search(r"Data size:\s+4294967296\b", data_attr):
            raise SystemExit("ntfsinfo did not confirm the 4-GiB $DATA high DWORD")
        if not re.search(r"Initialized size:\s+\d+\b", data_attr):
            raise SystemExit("ntfsinfo did not report initialized size for the oversized stream")
        oversized_before = digest(oversized_image)
        native = subprocess.run([str(HARNESS), str(oversized_image), "--ntfs-large-refuse"],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(native.stdout.decode(errors="replace"), end="", flush=True)
        if native.returncode or digest(oversized_image) != oversized_before:
            raise SystemExit("NTFS file with nonzero 64-bit size high word was not safely refused")
        print("PASS 4-GiB NTFS file size refused instead of 32-bit low-word alias; read-only SHA unchanged", flush=True)

        # Force a real two-run nonresident stream without mounting the image:
        # target cluster, blocker cluster, then ntfsfallocate extends target.
        frag_image = work / "ntfs-fragmented.img"
        fd = os.open(frag_image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        with os.fdopen(fd, "r+b") as out:
            out.truncate(IMAGE_BYTES)
        run([mkntfs, "--quick", "--force", "--sector-size", "512",
             "--label", "KVFRAG", str(frag_image)], env=env)
        prefix = work / "fragmented-prefix.bin"
        # A sub-cluster prefix makes Initialized Size end inside an allocated
        # cluster, exercising the exact boundary (not only whole-cluster tails).
        initialized_bytes = 1234
        prefix_payload = bytes((i * 13 + 7) % 251 for i in range(initialized_bytes))
        prefix.write_bytes(prefix_payload)
        blocker = work / "fragmentation-blocker.bin"
        blocker.write_bytes(b"B" * 4096)
        run([ntfscp, str(frag_image), str(prefix), "/fragmented.bin"], env=env)
        run([ntfscp, str(frag_image), str(blocker), "/blocker.bin"], env=env)
        run([ntfsfallocate, "-l", str(LARGE_BYTES - initialized_bytes), "-o", str(initialized_bytes),
             str(frag_image), "/fragmented.bin"], env=env)

        frag_info = run([ntfsinfo, "-F", "/fragmented.bin", str(frag_image)],
                        env=env, capture=True).stdout.decode(errors="replace")
        data_attr = frag_info.split("Dumping attribute $DATA", 1)[-1]
        if not re.search(r"Resident:\s+No", data_attr):
            raise SystemExit("fragmented fixture $DATA is unexpectedly resident")
        if not re.search(rf"Initialized size:\s+{initialized_bytes}\b", data_attr):
            raise SystemExit("fragmented fixture does not have the expected sub-cluster initialized size")
        cluster_info = run([ntfscluster, "-F", "/fragmented.bin", str(frag_image)],
                           env=env, capture=True).stdout.decode(errors="replace")
        if "VCN" not in cluster_info or "LCN" not in cluster_info:
            raise SystemExit("ntfscluster did not report the fragmented file runlist")
        run_rows = [(int(v), int(l), int(n)) for v, l, n in
                    re.findall(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s*$", cluster_info, re.M)]
        if len(run_rows) < 2 or run_rows[0][0] != 0:
            raise SystemExit(f"fragmented fixture has fewer than two data runs: {cluster_info}")
        next_vcn = 0
        has_physical_gap = False
        for i, (vcn, lcn, count) in enumerate(run_rows):
            if vcn != next_vcn or count <= 0:
                raise SystemExit(f"fragmented fixture runlist has a gap/invalid run: {run_rows}")
            next_vcn = vcn + count
            if i and run_rows[i - 1][1] + run_rows[i - 1][2] != lcn:
                has_physical_gap = True
        if not has_physical_gap or next_vcn != LARGE_BYTES // 4096:
            raise SystemExit(f"fixture is not a complete, physically fragmented 1MiB stream: {run_rows}")

        # Poison every allocated byte at/after Initialized Size, including
        # the suffix inside VCN 0. NTFS must never expose this stale data.
        cluster_size = 4096
        poison = bytes([0xA5]) * cluster_size
        with frag_image.open("r+b") as out:
            for vcn, lcn, count in run_rows:
                for index in range(count):
                    file_off = (vcn + index) * cluster_size
                    if file_off + cluster_size > initialized_bytes:
                        poison_off = max(0, initialized_bytes - file_off)
                        out.seek((lcn + index) * cluster_size + poison_off)
                        out.write(poison[poison_off:])
        expected_fragmented = work / "fragmented-expected.bin"
        expected_fragmented.write_bytes(prefix_payload + bytes(LARGE_BYTES - len(prefix_payload)))
        independent = run([ntfscat, str(frag_image), "/fragmented.bin"],
                          env=env, capture=True).stdout
        if independent != expected_fragmented.read_bytes():
            raise SystemExit("independent ntfscat did not zero-fill uninitialized fragmented data")

        before = digest(frag_image)
        native = subprocess.run([str(HARNESS), str(frag_image), "--ntfs-image",
                                 str(expected_fragmented), "0", "fragmented.bin"],
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(native.stdout.decode(errors="replace"), end="", flush=True)
        if native.returncode:
            raise SystemExit(f"production NTFS adapter failed fragmented/uninitialized fixture: exit {native.returncode}")
        if digest(frag_image) != before:
            raise SystemExit("read-only adapter changed fragmented fixture image")
        print(f"PASS fragmented nonresident image: {len(run_rows)} runs, Initialized size={initialized_bytes} (sub-cluster), poisoned tail zero-filled, SHA unchanged", flush=True)

    print("PASS NTFS private-image qualification: only temporary regular files; no host mount or block device", flush=True)


if __name__ == "__main__":
    main()
