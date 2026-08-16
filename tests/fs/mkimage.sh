#!/bin/sh
# Build a disk image holding a FAT32 and an NTFS partition, filled with
# the files the tests expect. The filesystems are created by the system
# tools on purpose: the drivers are then validated against somebody
# else's implementation rather than our own.
#
# Needs: sfdisk, mkfs.vfat (dosfstools), mkntfs (ntfs-3g), mtools, sudo
set -e
OUT=${1:-build/fsdisk.img}
PATH=$PATH:/sbin:/usr/sbin
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

dd if=/dev/zero of="$TMP/fat.part"  bs=512 count=532480 status=none
dd if=/dev/zero of="$TMP/ntfs.part" bs=512 count=513024 status=none
mkfs.vfat -F 32 -n KVANTDATA "$TMP/fat.part" >/dev/null
mkntfs -F -Q -L WINDATA "$TMP/ntfs.part" >/dev/null

# ---- the FAT32 side, through mtools (no mount needed) ----
cat > "$TMP/mtoolsrc" <<MT
drive z: file="$TMP/fat.part"
MT
export MTOOLSRC="$TMP/mtoolsrc"

printf 'Hello from FAT32! KvantOS reads this.\n' > "$TMP/hello.txt"
seq 1 400 | sed 's/^/Line /; s/$/ of a longer test file/' > "$TMP/big.txt"
mmd z:/Documents z:/Music
mcopy "$TMP/hello.txt" z:/HELLO.TXT
mcopy "$TMP/big.txt" "z:/Documents/A very long file name.txt"
mcopy "$TMP/big.txt" z:/Music/track01.dat

# ---- the NTFS side, through a loop mount ----
mkdir -p "$TMP/mnt"
sudo mount -o loop,rw "$TMP/ntfs.part" "$TMP/mnt"
sudo mkdir -p "$TMP/mnt/Windows" "$TMP/mnt/Users/Public" "$TMP/mnt/Program Files"
printf 'This file lives on a real NTFS volume, read by KvantOS.\n' \
    | sudo tee "$TMP/mnt/readme.txt" >/dev/null
seq 1 500 | sed 's/^/NTFS test line /; s/$/ with UTF-8 text/' \
    | sudo tee "$TMP/mnt/Users/Public/notes.txt" >/dev/null
printf 'A short file with a Cyrillic name\n' \
    | sudo tee "$TMP/mnt/Users/Privet.txt" >/dev/null
# a large file: guaranteed non-resident, spanning several runs
sudo dd if=/dev/urandom of="$TMP/mnt/Windows/bigdata.bin" bs=1K count=900 status=none
# a tiny one: its data fits inside the MFT record (resident $DATA)
printf 'tiny\n' | sudo tee "$TMP/mnt/Windows/tiny.txt" >/dev/null
# many entries, so the directory grows an $INDEX_ALLOCATION B-tree
i=1
while [ $i -le 120 ]; do
    printf 'file %s\n' "$i" | sudo tee "$TMP/mnt/Program Files/document_number_$i.txt" >/dev/null
    i=$((i + 1))
done
sudo cp "$TMP/mnt/Windows/bigdata.bin" ./mnt_bigdata.ref
sudo umount "$TMP/mnt"
sudo chown "$(id -u):$(id -g)" mnt_bigdata.ref

# ---- assemble the disk ----
dd if=/dev/zero of="$OUT" bs=1M count=513 status=none
sfdisk "$OUT" >/dev/null <<PART
label: dos
start=2048, size=532480, type=c
start=534528, size=513024, type=7
PART
dd if="$TMP/fat.part"  of="$OUT" bs=512 seek=2048   conv=notrunc status=none
dd if="$TMP/ntfs.part" of="$OUT" bs=512 seek=534528 conv=notrunc status=none
echo "built $OUT"
