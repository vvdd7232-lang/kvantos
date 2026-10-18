#!/usr/bin/env bash
# Optional, disposable HS-only MODEL, not a USB2 TT implementation.
# Build files stay under build/hs-qemu; stock QEMU/usb-hub are NOT replaced.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
work="$root/build/hs-qemu"
mkdir -p "$work"
cd "$work"
archive=qemu-10.0.0.tar.xz
[[ -f "$archive" ]] || curl -fL --retry 2 "https://download.qemu.org/$archive" -o "$archive"
echo "22c075601fdcf8c7b2671a839ebdcef1d4f2973eb6735254fd2e1bd0f30b3896  $archive" | sha256sum -c -
# Re-extraction intentionally restores this managed scratch source, not user code.
tar -xf "$archive"
patch -d qemu-10.0.0 -p1 < "$root/tests/boot/fixtures/qemu-10.0.0-hs-hub.patch"
mkdir -p build
cd build
../qemu-10.0.0/configure --target-list=x86_64-softmmu --disable-docs --disable-tools \
  --disable-user --disable-werror --disable-gtk --disable-sdl --disable-vnc \
  --disable-opengl --audio-drv-list= --disable-guest-agent --disable-plugins --disable-slirp
ninja -j2 qemu-system-x86_64
./qemu-system-x86_64 -device help | grep kvantos-lab-hs-hub
printf '\nExplicit LAB executable: %s/build/qemu-system-x86_64\nNO TT or physical qualification.\n' "$work"
