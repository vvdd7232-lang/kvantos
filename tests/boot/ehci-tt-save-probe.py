#!/usr/bin/env python3
"""Bounded save diagnostic, explicit TT LAB, NORMAL kernel, private images only.
Reuses save-as-smoke's fixture/UI helpers (prefix before its first guest).
No product RAM edits; optional finite write throttling is EXPLICIT fault injection.
RUN_ID isolates all paths for parallel workers. Never relaxes driver limits.
"""
from pathlib import Path
import json,os,subprocess,sys,time,gzip,shutil
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
assert os.environ.get('USB_TT_LAB')=='1' and os.environ.get('GUI_SAVE_OWNED')=='1'
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
RUN_ID=os.environ.get('RUN_ID','probe'+os.environ['MCC_ARCH64'])
assert 0<len(RUN_ID)<=16 and all(c.isalnum() or c in '-_' for c in RUN_ID)
RUNS=int(os.environ.get('TT_PROBE_RUNS','3'));assert 1<=RUNS<=20
SLOW=os.environ.get('TT_PROBE_SLOW')=='1'
source_py=ROOT/'tests/boot/save-as-smoke.py'
text=source_py.read_text();end=text.index('old=external()')+len('old=external()')
prefix=text[:end].replace('smoke.WORK=topology.work(smoke.WORK)',"smoke.WORK=ROOT/'build/tt-probe'/"+repr(RUN_ID))
# Helpers remain compiled with their original filename/line numbers.
ns={'__file__':str(source_py),'__name__':'tt_probe_helpers'}
exec(compile(prefix,str(source_py),'exec'),ns)
from ehci_fault import faults,dump_state
smoke=ns['smoke'];WORK=smoke.WORK
report={'mode':'2 write IOPS' if SLOW else 'unforced','runs_requested':RUNS,'arch64':smoke.IS64,'run_id':RUN_ID,'attempts':[]}
expected=dict(ns['old']);failed=False
a=ns['args']()
if SLOW:
    at=a.index('-drive')+1;a[at]+=',throttling.iops-write=2'
with smoke.Guest(extra_args=a) as q:
    ns['start'](q);ns['open_local'](q,'huge.txt')
    assert q.document()['text']==ns['large']
    for i in range(RUNS):
        name=f'Large-{i}.txt';ns['picker'](q,ns['vol']);ns['choose'](q,'Empty long directory');q.choose_name(name)
        before=ns['snap'](q);target=bytes(ns['state'](q).target)
        began=time.monotonic();ns['accept'](q)
        state=ns['state'](q);ok=bool(state.target_valid and not state.target_failed and state.target.decode()==ns['vol']+'/Empty long directory/'+name)
        d=dict(index=i,success=ok,elapsed=time.monotonic()-began,status=bytes(state.status).decode(),faults=faults(q))
        report['attempts'].append(d);print(json.dumps(d,ensure_ascii=False),flush=True)
        if not ok:
            failed=True
            assert ns['snap'](q)==before and bytes(state.target)==target,'failure mutated document/history/previous target'
            if SLOW:assert d['faults'],'controlled transport timeout needs a captured HC record'
            q.cmd('stop');d['snapshots']=dump_state(q,WORK);q.cmd('cont')
            (WORK/'failure.serial.log').write_bytes(q.log.read_bytes())
            (WORK/'failure-editor.bin').write_bytes(q.mem('note_editor',ns['C'].sizeof(ns['Editor'])))
            break
        assert not SLOW,'finite slow I/O must exercise the timeout, not report success'
        expected['Empty long directory/'+name]=ns['large']
    (WORK/'final.serial.log').write_bytes(q.log.read_bytes())
report['source_sha256']=ns['digest'](ns['source'])
if failed:
    with ns['source'].open('rb') as src,gzip.open(WORK/'source.img.gz','wb') as dst:shutil.copyfileobj(src,dst)
    report['first']=ns['first'];report['total']=ns['total'];report['bytes']=ns['source'].stat().st_size
    report['complete_file']='Empty long directory/'+name
    (WORK/'observation.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
    (WORK/'expected-namespace.json').write_text(json.dumps({k:None if v is None else v.hex() for k,v in expected.items()},indent=2)+'\n')
else:assert ns['external']()==expected,'successful saves failed cold host fsck/namespace/data check'
(WORK/'probe.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
if not SLOW:assert not failed,'UNFORCED failure captured; inspect probe.json and trace, do not relabel as pass'
else:assert failed,'finite slow backing I/O was not exercised'
print('PASS probe',RUN_ID,':','controlled failure + caller protection' if failed else 'all saves + host fsck/exact namespace',flush=True)
