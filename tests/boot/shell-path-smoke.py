#!/usr/bin/env python3
"""Actual bounded shell paths and read/delete EIO on private ATA images."""
from pathlib import Path
import os,sys,subprocess,time,json,hashlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
sys.path.insert(0,str(ROOT/'sdk'));from mkdisk import KvFS,DIR_SECTOR,DATA_START
smoke.WORK=ROOT/'build/shell-path'/('x64' if smoke.IS64 else 'i386');smoke.WORK.mkdir(parents=True,exist_ok=True)
def command(q,cmd,message):
    offset=len(q.log.read_text(errors='replace'));q.text(cmd+'\n');end=time.monotonic()+10
    while message not in q.log.read_text(errors='replace')[offset:]:
        assert time.monotonic()<end,(cmd,message,q.log.read_text(errors='replace')[-600:]);time.sleep(.025)
    return q.log.read_text(errors='replace')[offset:]
def digest():return hashlib.sha256(image.read_bytes()).hexdigest()
def capture(q,name):(smoke.WORK/name).write_text(q.log.read_text(errors='replace'))
image=smoke.WORK/'normal.img';smoke.DISK=image;k=KvFS(str(image),16);k.add('proof.txt',b'SHELL-PATH-PROOF',False);k.add('empty.txt',b'',False)
with smoke.Guest() as q:
    before=digest();command(q,'vcd /mnt/kv','  /mnt/kv');command(q,'vcat proof.txt','SHELL-PATH-PROOF')
    command(q,'vcat empty.txt','kvant:/');assert digest()==before
    command(q,'vcd proof.txt','Not a directory:');command(q,'vcd','Current: /mnt/kv')
    command(q,'vmkdir folder','This volume is read-only');assert digest()==before
    command(q,'vrm missing.txt','Could not delete');assert digest()==before
    command(q,'vrm proof.txt','Deleted: /mnt/kv/proof.txt');assert {e['name'] for e in KvFS(str(image)).listing()}=={'empty.txt'}
    command(q,'vcd ..','Not a directory:');command(q,'vcd','Current: /mnt/kv');capture(q,'normal.serial.log')
with smoke.Guest() as q:assert q.num('mounted')==1 and {e['name'] for e in KvFS(str(image)).listing()}=={'empty.txt'}
for case,event,sector in [('read','read_aio',DATA_START),('delete','write_aio',DIR_SECTOR)]:
    image=smoke.WORK/(case+'.img');k=KvFS(str(image),16);k.add('proof.txt',b'SHELL-PATH-PROOF',False)
    config=smoke.WORK/(case+'.conf');config.write_text('[inject-error]\nevent = "'+event+'"\nerrno = "5"\nonce = "on"\nimmediately = "on"\nsector = "'+str(sector)+'"\n');smoke.DISK=f'blkdebug:{config}:{image}'
    with smoke.Guest() as q:
        before=digest();output=command(q,('vcat' if case=='read' else 'vrm')+' /mnt/kv/proof.txt','Incomplete read or storage changed; nothing displayed' if case=='read' else 'Delete outcome uncertain; remount and inspect')
        assert digest()==before and q.num('mounted')==(case=='read') and 'SHELL-PATH-PROOF' not in output and 'Deleted:' not in output
        capture(q,case+'.serial.log')
    smoke.DISK=image
    with smoke.Guest() as q:assert q.num('mounted')==1 and {e['name'] for e in KvFS(str(image)).listing()}=={'proof.txt'}
result={'arch64':smoke.IS64,'checks':'relative vcat/empty, cd valid/file/up refusal/CWD retention, unsupported mkdir/missing remove unchanged, successful relative remove cold persistence; read EIO no contents, metadata delete EIO no false success/namespace fence/cold original','scope':'overcount/long CWD/namespace during callbacks host-only; no atomic multiwriter/physical storage qualification'}
(smoke.WORK/'result.json').write_text(json.dumps(result,indent=2));print('PASS bounded shell paths','UEFI64' if smoke.IS64 else 'BIOS32',result['checks'],flush=True)
