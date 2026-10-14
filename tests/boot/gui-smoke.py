#!/usr/bin/env python3
"""Exercise both main-ISO kernels through real keyboard/mouse input in QEMU."""
from pathlib import Path
import os
import shutil
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
WORK = ROOT / 'build/gui-smoke'
WORK.mkdir(parents=True, exist_ok=True)


def run(arch):
    folder = WORK / arch
    folder.mkdir(exist_ok=True)
    log = folder / 'serial.log'
    log.unlink(missing_ok=True)
    monitor = folder / 'monitor.sock'
    monitor.unlink(missing_ok=True)
    kernel = 'build/kvant64.bin' if arch == 'uefi64' else 'build/kvant.bin'
    syms = {}
    for line in subprocess.check_output(['nm', '-n', kernel], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    firmware = []
    if arch == 'uefi64':
        code = os.environ.get('OVMF_CODE', '/usr/share/OVMF/OVMF_CODE_4M.fd')
        template = os.environ.get('OVMF_VARS', '/usr/share/OVMF/OVMF_VARS_4M.fd')
        variables = folder / 'vars.fd'
        shutil.copyfile(template, variables)
        firmware = ['-drive', f'if=pflash,format=raw,readonly=on,file={code}',
                    '-drive', f'if=pflash,format=raw,file={variables}']
    proc = subprocess.Popen(['qemu-system-x86_64', '-machine',
                             'q35,accel=tcg' if firmware else 'pc,accel=tcg',
                             '-cpu', 'max' if firmware else 'pentium', '-m', '256',
                             *firmware, '-cdrom', 'release/kvantos.iso', '-boot', 'd',
                             '-display', 'none', '-serial', 'file:' + str(log),
                             '-monitor', 'unix:' + str(monitor) + ',server,nowait',
                             '-no-reboot'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    sock = None
    try:
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if log.exists() and 'kvant:/$ ' in log.read_text(errors='replace'):
                break
            if proc.poll() is not None:
                raise RuntimeError(proc.stderr.read().decode())
            time.sleep(.2)
        else:
            raise RuntimeError(log.read_text(errors='replace'))
        text = log.read_text(errors='replace')
        assert 'KvantOS 3.1.0' in text, text
        sock = socket.socket(socket.AF_UNIX)
        sock.settimeout(5)
        sock.connect(str(monitor))

        def response():
            data = b''
            while b'(qemu) ' not in data:
                part = sock.recv(65536)
                if not part:
                    raise RuntimeError('QEMU monitor closed')
                data += part
            return data

        response()

        def cmd(command):
            sock.sendall((command + '\n').encode())
            return response()

        def mem(symbol, size):
            address = syms[symbol] if isinstance(symbol, str) else symbol
            path = folder / 'memory.bin'
            path.unlink(missing_ok=True)
            cmd(f'pmemsave 0x{address:x} {size} "{path}"')
            return path.read_bytes()

        def num(symbol):
            return struct.unpack('<i', mem(symbol, 4))[0]

        def key(name):
            cmd('sendkey ' + name + ' 40')
            time.sleep(.12)

        for c in 'guimenu':
            key(c)
        key('ret')
        time.sleep(1.5)
        assert num('running') == 1
        assert num('scr_w') == 1024 and num('scr_h') == 768
        cmd(f'screendump "{folder / "desktop.ppm"}"')
        key('g')
        time.sleep(.5)
        stride = 40 if firmware else 36
        table = mem('files', 32 * stride)
        expected = {p.name.encode() for p in Path('release/apps').glob('*.kapp')}
        actual = {table[i * stride:i * stride + 24].split(b'\0')[0] for i in range(32)}
        assert len(expected) == 11 and expected.issubset(actual), actual
        target = None
        for i in range(32):
            name = table[i * stride:i * stride + 24].split(b'\0')[0]
            if name == b'maze3d.kapp':
                target = 400 + i
                break
        assert target is not None
        hit = None
        for x, y, w, h, identifier, window in struct.iter_unpack('<6i', mem('hits', num('hit_count') * 24)):
            if identifier == target:
                hit = x + w // 2, y + h // 2
                break
        assert hit, 'Maze 3D launcher row is not visible'
        # Read actual guest coordinates; never blindly retry mouse aiming.
        for attempt in range(12):
            x, y = num('mx'), num('my')
            dx, dy = hit[0] - x, hit[1] - y
            if abs(dx) < 5 and abs(dy) < 5:
                break
            dx, dy = max(-80, min(80, dx)), max(-80, min(80, dy))
            cmd(f'mouse_move {dx} {dy}')
            time.sleep(.1)
        else:
            raise RuntimeError('Could not reach measured launcher row')
        cmd('mouse_button 1')
        time.sleep(.2)
        cmd('mouse_button 0')
        time.sleep(1)
        assert num('loaded') == 1, mem('last_error', 96)
        assert mem('app_file', 40).split(b'\0')[0] == b'maze3d.kapp'
        t0 = struct.unpack('<Q', mem('ticks', 8))[0]
        key('up')
        key('right')
        time.sleep(.5)
        assert num('loaded') == 1 and num('app_faulted') == 0
        assert struct.unpack('<Q', mem('ticks', 8))[0] > t0
        cmd(f'screendump "{folder / "maze3d.ppm"}"')
        for _ in range(4):
            key('esc')
        time.sleep(.5)
        assert num('running') == 0
        assert 'Back in the kvsh text console.' in log.read_text(errors='replace')
        assert 'KERNEL PANIC' not in log.read_text(errors='replace')
        print(f'{arch}: 11 apps + 1024x768 GUI + native Maze 3D/input + timer + shell return PASS')
    finally:
        if sock:
            sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == '__main__':
    run('bios32')
    run('uefi64')
