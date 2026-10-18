#!/usr/bin/env python3
"""Native CPU CPL3 fixtures, not a claim of public KAPP ABI4 isolation."""
from pathlib import Path
import os,sys,subprocess,time,json
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
smoke.WORK=ROOT/'build/user-isolation'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
with smoke.Guest(disk=False) as q:
    # Repeated full fixture runs must restore the original CR3/TSS and release frames.
    reports=[]
    for run in range(3):
        begin=len(q.log.read_bytes());q.text('isotest\n');deadline=time.monotonic()+30
        while b'opt-in ABI4 uses the separate strict image loader' not in q.log.read_bytes()[begin:]:
            assert time.monotonic()<deadline,q.log.read_text(errors='replace')[-5000:]
            time.sleep(.03)
        output=q.log.read_bytes()[begin:];assert b'[userlab] ALL PASS failures=0' in output,output.decode(errors='replace')
        assert b'[userlab] FAIL' not in output
        for case in (b'exit',b'kernel-write',b'null-write',b'guard-write',b'RX-write',b'CLI',b'OUT',b'HLT',b'UD2',b'divide-zero',b'kernel-read',b'infinite-loop',b'copy-good',b'copy-wrap',b'copy-RX',b'copy-version',b'stack-overflow',b'read-CR3',b'copy-unknown',b'copy-header-size',b'copy-header-wrap',b'copy-quota',b'copy-empty',b'event-wire',b'draw-clipped',b'RAM-capabilities',b'service-generation',b'stale-handle',b'service-flags',b'kill-suspended',b'resume-registers',b'FPU-disabled',b'FWAIT-disabled',b'FNINIT-disabled',b'FNSTENV-disabled',b'FNSAVE-disabled',b'MMX-disabled',b'SSE-disabled',b'STMXCSR-disabled',b'LDMXCSR-disabled',b'FXSAVE-disabled',b'FXRSTOR-disabled',b'XSAVE-disabled',b'XRSTOR-disabled'):
            assert b'[userlab] PASS '+case+b' ' in output,(case,output)
        if smoke.IS64:
            for case in (b'NX-data',b'noncanonical',b'syscall-upper-half',b'syscall-disabled',b'fsgsbase-disabled',b'XSAVE-inherited-denied',b'XRSTOR-inherited-denied'):
                assert b'[userlab] PASS '+case+b' ' in output,(case,output)
        reports.append(output.decode(errors='replace'))
    begin=len(q.log.read_bytes());q.text('help\n');deadline=time.monotonic()+5
    while b'KvantOS commands' not in q.log.read_bytes()[begin:] or b'kvant:/$' not in q.log.read_bytes()[begin:]:
        assert time.monotonic()<deadline;time.sleep(.03)
    (smoke.WORK/'result.json').write_text(json.dumps({'arch64':smoke.IS64,'runs':reports,'scope':'built-in CPL3 diagnostics incl. FSGSBASE and x87/MMX/SSE/FXSAVE/XSAVE denial; no FP context or floats enabled; public ABI4 tested separately'},indent=2))
print('PASS native CPL3','UEFI64' if smoke.IS64 else 'BIOS32','3 runs, fault/privilege/guard/RX/NX/copy/timer/teardown + shell alive',flush=True)
