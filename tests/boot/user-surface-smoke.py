#!/usr/bin/env python3
"""Protocol2 shared pixels through real GUI/CPL3, with deliberate RO fault.
Private regular-file guest disk only; requires built ISO and SDK pixels apps.
"""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import time
ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0', '1'):
        subprocess.run([sys.executable, __file__], env=dict(os.environ, MCC_ARCH64=arch), check=True)
    raise SystemExit
sys.path.insert(0, str(ROOT / 'tests/mcc'))
import smoke
smoke.WORK = ROOT / 'build/user-surface' / ('x64' if smoke.IS64 else 'i386')
smoke.WORK.mkdir(parents=True, exist_ok=True)
smoke.DISK = smoke.WORK / 'surface.img'
subprocess.run([sys.executable, 'sdk/mkdisk.py', str(smoke.DISK), '16'], check=True)
sys.path.insert(0, str(ROOT / 'sdk'))
from mkdisk import KvFS
sdk = ROOT / 'sdk/isolated/build' / ('pixels64.kapp' if smoke.IS64 else 'pixels32.kapp')
KvFS(str(smoke.DISK)).add('pixels.kapp', sdk.read_bytes(), True)
before = hashlib.sha256(smoke.DISK.read_bytes()).hexdigest()


def until(test, what, timeout=15):
    deadline = time.monotonic() + timeout
    while not test():
        assert time.monotonic() < deadline, what
        time.sleep(.03)


with smoke.Guest() as guest:
    guest.text('guimenu\n')
    time.sleep(.5)
    guest.open_app(8)
    baseline = guest.num('used_frames')

    def launch():
        guest.open_app(8)
        guest.widget(210, 8)
        until(lambda: guest.num('app_resident') and guest.num('app_surface_commits') >= 2, 'shared present')
        assert guest.num('isolated') and not guest.num('app_faulted')
        assert guest.num('used_frames') >= baseline + 248
        guest.open_app(10)

    launch()
    guest.screenshot(smoke.WORK / 'shared-alpha.png')
    guest.close_app(10)
    until(lambda: not guest.num('app_resident'), 'normal surface close')
    assert guest.num('used_frames') == baseline, 'normal close leaks shared frames'
    launch()
    guest.key('f')
    until(lambda: not guest.num('app_resident'), 'front write must fault in CPL3')
    assert guest.num('app_faulted'), 'front write was not trapped'
    assert b'vector 14' in guest.mem('last_error', 96), 'expected write protection page fault'
    assert guest.num('used_frames') == baseline, 'fault cleanup leaks shared frames'
    guest.exit_gui()
    guest.text('cat /proc/meminfo\n')
    until(lambda: 'MemTotal:' in guest.log.read_text(errors='replace'), 'shell alive after fault')
    (smoke.WORK / 'serial.log').write_text(guest.log.read_text(errors='replace'))
assert hashlib.sha256(smoke.DISK.read_bytes()).hexdigest() == before
print('PASS shared pixels, alpha render path, immutable-front CPL3 fault, close/fault frame cleanup, immutable source disk', 'x64' if smoke.IS64 else 'i386')
