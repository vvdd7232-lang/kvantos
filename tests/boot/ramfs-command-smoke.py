#!/usr/bin/env python3
"""Actual shell cp/mv on diskless production kernels; OOM is host-only."""
from pathlib import Path
import os,sys,subprocess,time,json
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
smoke.WORK=ROOT/'build/ramfs-commands'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
def command(q,cmd,message):
    offset=len(q.log.read_text(errors='replace'));q.text(cmd+'\n');deadline=time.monotonic()+8
    while message not in q.log.read_text(errors='replace')[offset:]:
        assert time.monotonic()<deadline,(cmd,message,q.log.read_text(errors='replace')[-400:]);time.sleep(.025)
with smoke.Guest(disk=False) as q:
    files=q.files();source=files['readme.txt'];before=q.files()
    command(q,'cp readme.txt readme.txt','source and destination are the same; unchanged');assert q.files()==before
    command(q,'cp readme.txt license.txt',"'readme.txt' -> 'license.txt'");assert q.files()['license.txt']==source and q.files()['readme.txt']==source
    before=q.files();command(q,'cp missing.txt license.txt',"file 'missing.txt' not found");assert q.files()==before
    command(q,'cp readme.txt '+'x'*24,'target unchanged');assert q.files()==before
    command(q,'touch empty.txt',"file 'empty.txt' created (empty)")
    command(q,'cp empty.txt license.txt',"'empty.txt' -> 'license.txt' (0 bytes)");assert q.files()['license.txt']==b'' and q.files()['readme.txt']==source
    for i in range(32-len(q.files())):q.text(f'touch f{i}\n')
    assert len(q.files())==32
    command(q,'cp readme.txt license.txt',"'readme.txt' -> 'license.txt'");assert q.files()['license.txt']==source
    before=q.files();command(q,'cp readme.txt new.txt','target unchanged');assert q.files()==before
    command(q,'mv readme.txt renamed.txt',"'readme.txt' renamed to 'renamed.txt'")
    assert 'readme.txt' not in q.files() and q.files()['renamed.txt']==source and len(q.files())==32
    before=q.files();command(q,'mv renamed.txt license.txt','target exists; neither file replaced');assert q.files()==before
    (smoke.WORK/'serial.log').write_text(q.log.read_text(errors='replace'))
result={'arch64':smoke.IS64,'checks':'diskless actual cp samepath preserves bytes, overwrite+source retention, missing/long/full-table refusal, empty copy, full-table no-replace mv and conflict; shell alive','scope':'OOM target retention and no-allocation counters host-only; no physical disks or VFS crossvolume copy/move'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2))
print('PASS RAM shell cp/mv','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
