#!/bin/bash
# ============================================================
#  KvantOS - embed applications into the boot image
#
#  The simplest way to deliver a .kapp to the system: put it inside
#  the ISO. No disk, no network and no flash drive are needed - the
#  program is there right after boot.
#
#  Usage:
#      ./sdk/addapp.sh my_game.kapp [more.kapp ...]
#
#  Result: release/kvantos.iso and release/kvantos-floppy.img with
#  your applications inside.
# ============================================================
set -e

cd "$(dirname "$0")/.."          # project root

if [ $# -eq 0 ]; then
    echo "Usage: ./sdk/addapp.sh file.kapp [file2.kapp ...]"
    echo
    echo "The applications will be embedded into the boot image."
    echo "Currently in the image:"
    ls -1 release/apps/*.kapp 2>/dev/null | sed 's|release/apps/|  |' || echo "  (empty)"
    exit 1
fi

mkdir -p release/apps

for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "  ERROR: file '$f' not found"
        exit 1
    fi
    # Check the signature so that a wrong file is not embedded by accident
    sig=$(head -c 4 "$f")
    if [ "$sig" != "KAPP" ]; then
        echo "  ERROR: '$f' is not a KvantOS application (no KAPP signature)"
        echo "  Build it with sdk/mkkapp.py"
        exit 1
    fi
    name=$(basename "$f")
    cp "$f" "release/apps/$name"
    echo "  + $name ($(stat -c%s "$f") bytes)"
done

# grub.cfg lists the modules by name - regenerate that list
python3 - <<'PY'
import glob, os, re

apps = sorted(os.path.basename(p) for p in glob.glob("release/apps/*.kapp"))
lines = "".join(f"    module /boot/apps/{a} {a}\n" for a in apps)

cfg = open("grub/grub.cfg").read()
# drop the old module lines and insert fresh ones after each multiboot
cfg = re.sub(r"^ *module /boot/apps/.*\n", "", cfg, flags=re.M)
cfg = re.sub(r"(    multiboot /boot/kvant\.bin(?: text)?\n)(?!.*safe)",
             lambda m: m.group(1) + lines, cfg)
open("grub/grub.cfg", "w").write(cfg)
print(f"  grub.cfg: {len(apps)} application(s) in the menu")
PY

echo "  Rebuilding the images..."
touch kernel/main.c
make >/dev/null 2>&1
rm -f build/kvantos.iso
make iso  >/dev/null 2>&1
make floppy >/dev/null 2>&1

echo
echo "  DONE."
echo "    release/kvantos.iso          - burn to a disc/USB stick or attach in VMware"
echo "    release/kvantos-floppy.img   - the fallback option"
echo
echo "  Inside the system: guimenu -> key G -> your program is in the list."
