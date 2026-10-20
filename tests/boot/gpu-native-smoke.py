#!/usr/bin/env python3
"""Kernel-native GPU bring-up: the GRUB entry used here deliberately
leaves the bootloader in VGA text (gfxpayload=text, no framebuffer
hand-over). The kernel itself must program the Bochs VBE adapter into
1024x768x32, move the console onto the GPU framebuffer and reach kvsh
with a non-black graphics screen."""
from pathlib import Path
import os, re, socket, subprocess, time

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
WORK = ROOT / 'build/gpu-native-smoke'
WORK.mkdir(parents=True, exist_ok=True)
LOG = WORK / 'serial.log'
MON = WORK / 'monitor.sock'
SHOT = WORK / 'screen.ppm'
for p in (LOG, MON, SHOT):
    p.unlink(missing_ok=True)

proc = subprocess.Popen(
    ['qemu-system-x86_64', '-machine', 'pc,accel=tcg', '-cpu', 'pentium',
     '-m', '256', '-vga', 'std', '-cdrom', 'release/kvantos.iso',
     '-boot', 'd', '-display', 'none', '-serial', 'file:' + str(LOG),
     '-monitor', 'unix:' + str(MON) + ',server,nowait', '-no-reboot'],
    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def mon_connect():
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(str(MON))
            s.settimeout(5)
            s.recv(4096)
            return s
        except OSError:
            time.sleep(.2)
    raise RuntimeError('monitor socket timeout')


def cmd(sock, text):
    sock.sendall((text + '\n').encode())
    time.sleep(.15)
    try:
        return sock.recv(65536)
    except socket.timeout:
        return b''


try:
    sock = mon_connect()
    # GRUB menu: entry 5 is "kernel GPU driver (bootloader stays in text)".
    time.sleep(2)
    for _ in range(5):
        cmd(sock, 'sendkey down')
        time.sleep(.3)
    cmd(sock, 'sendkey ret')

    deadline = time.monotonic() + 120
    text = ''
    while time.monotonic() < deadline:
        if LOG.exists():
            text = LOG.read_text(errors='replace')
            if 'kvant:/$ ' in text:
                break
        if proc.poll() is not None:
            raise RuntimeError(proc.stderr.read().decode())
        time.sleep(.25)
    else:
        raise RuntimeError('no kvsh prompt:\n' + text[-3000:])

    assert 'GPU mode set by the kernel driver' in text, text[-3000:]
    assert re.search(r'(framebuffer|фреймбуфер) 1024x768', text), text[-3000:]

    # The display must be a real graphics frame, not leftover VGA text.
    cmd(sock, f'screendump {SHOT}')
    deadline = time.monotonic() + 15
    while not SHOT.exists() and time.monotonic() < deadline:
        time.sleep(.2)
    data = SHOT.read_bytes()
    header, _, pixels = data.partition(b'255\n')
    dims = header.split()[-2:]
    assert dims == [b'1024', b'768'], dims
    sample = [pixels[i:i + 3] for i in range(0, len(pixels) - 2, 3 * 101)]
    colours = set(sample)
    nonblack = sum(1 for px in sample if px != b'\x00\x00\x00')
    # A text console on the framebuffer is mostly black by design; what
    # proves a live graphics mode is the mode geometry (checked above)
    # plus drawn content: status bar, glyphs, the coloured logo.
    assert len(colours) >= 4, f'screen looks flat: {len(colours)} colours'
    assert nonblack > len(sample) // 100, f'screen looks blank: {nonblack} lit samples'
    print('PASS kernel-native GPU: bootloader stayed in text, kernel set '
          '1024x768x32 on the adapter, console+kvsh live on the framebuffer')
finally:
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
