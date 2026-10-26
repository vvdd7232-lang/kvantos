#!/usr/bin/env bash
# Package already-built, tested release artifacts without publishing anything.
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(awk '$2 == "KV_VERSION" {gsub(/"/, "", $3); print $3}' include/kernel.h)
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid kernel version' >&2; exit 1; }
tag=v$version
[[ $(git describe --exact-match --tags HEAD) == "$tag" ]] || {
    echo "Check out $tag before packaging" >&2; exit 1;
}
[[ -z $(git status --porcelain --untracked-files=no) ]] || {
    echo "Tracked source changes must be committed before packaging" >&2; exit 1;
}
python3 tests/boot/check-images.py
# A release must not contain a user's working filesystem.
python3 - <<'PY'
import sys
sys.path.insert(0, 'sdk')
from mkdisk import KvFS
assert not any(KvFS('release/kvantos-disk.img').entries), 'Refusing to ship a nonempty KvFS disk'
PY
mkdir -p packages
cp "kvantos-$version.tar.gz" packages/
cp release/kvantos.iso release/kvantos-direct.iso release/kvantos-floppy.img \
   release/kvantos-disk.img release/kvant.bin release/kvant64.bin \
   release/kvantefi.efi release/esp.img packages/
git archive --format=tar --prefix="kvantos-$version/" "$tag" | gzip -n > "packages/kvantos-$version-source.tar.gz"
cp RELEASE.md RELEASE.ru.md packages/
(
    cd packages
    sha256sum "kvantos-$version.tar.gz" "kvantos-$version-source.tar.gz" \
        kvantos.iso kvantos-direct.iso kvantos-floppy.img kvantos-disk.img \
        kvant.bin kvant64.bin kvantefi.efi esp.img > "kvantos-$version-SHA256SUMS.txt"
    sha256sum -c "kvantos-$version-SHA256SUMS.txt"
)
echo "Release files are in packages/. Nothing has been uploaded."
