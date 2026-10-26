#!/usr/bin/env python3
"""Actual procfs shell reads/RO/rescan on diskless BIOS32 and UEFI64 kernels.
Requires built kernels/ISO, QEMU and OVMF. Never opens a host block device.
"""
from pathlib import Path
import json
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

smoke.WORK = ROOT / 'build/procfs-smoke' / ('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True, exist_ok=True)


def command(guest, text, expected):
    offset = len(guest.log.read_text(errors='replace'))
    guest.text(text + '\n')
    deadline = time.monotonic() + 30
    while expected not in guest.log.read_text(errors='replace')[offset:]:
        assert time.monotonic() < deadline, (text, expected, guest.log.read_text(errors='replace')[-1500:])
        time.sleep(.025)


with smoke.Guest(disk=False) as guest:
    command(guest, 'cat /proc/cpuinfo', 'Architecture:')
    command(guest, 'cat /proc/meminfo', 'MemTotal:')
    command(guest, 'cat /mnt/proc/stat', 'TimerHz:')
    command(guest, 'cat /proc/0/status', 'Switches:')
    command(guest, 'vls /proc', 'cpuinfo')
    command(guest, 'vrm /proc/version', 'This volume is read-only')
    command(guest, 'vmkdir /proc/test', 'This volume is read-only')
    command(guest, 'vcp /mnt/ram/readme.txt /proc/version', 'The target is read-only')
    command(guest, 'rescan', 'procfs')
    command(guest, 'cat /proc/version', 'KvantOS')
    command(guest, 'cat /proc/00/status', 'File not found')
    command(guest, 'cat /proc/0/status', 'Switches:')
    (smoke.WORK / 'serial.log').write_text(guest.log.read_text(errors='replace'))
(smoke.WORK / 'result.json').write_text(json.dumps({
    'arch64': smoke.IS64,
    'checks': 'actual cat/VFS, CPU/memory/tasks, RO writes/remove/mkdir, proc survives rescan, bad PID alias rejected',
    'scope': 'diskless QEMU guest; no shared GUI buffers, kernel modules, physical hardware or ring3 file API'
}, indent=2))
print('PASS procfs', 'UEFI64' if smoke.IS64 else 'BIOS32', flush=True)
