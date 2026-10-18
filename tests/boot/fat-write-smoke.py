#!/usr/bin/env python3
"""Actual USB -> VFS -> FAT/WAL in an unpublishable lab ISO. Private files only.
Cold guest crash/restart, backend EIO, unplug/reconnect, external fsck + namespace.
No mount or host block devices. Run make STORAGE_LAB=1 iso first.
"""
from pathlib import Path
import os,sys,subprocess,struct,time,hashlib,shutil,zlib
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'))
import smoke
from mkfatlab import make
CONTROLLER=os.environ.get('USB_CONTROLLER','ehci-ohci');LAYOUT=os.environ.get('USB_LAYOUT','gpt')
assert CONTROLLER in ('uhci','ehci-ohci') and LAYOUT in ('mbr','gpt')
smoke.WORK=ROOT/'build/fat-usb'/CONTROLLER/LAYOUT/('uefi64' if smoke.IS64 else 'bios32')
smoke.WORK.mkdir(parents=True,exist_ok=True)
assert 'fat_lab_stage' in smoke.symbols and 'stx_lab_pause_at' in smoke.symbols,'STORAGE_LAB=1 required'
source,first,total,journal,bpb=make(smoke.WORK,LAYOUT)
size=source.stat().st_size
body=bytes((i*19+7)&255 for i in range(65535))
vol='usb0p2' if LAYOUT=='gpt' else 'usb0p1'
base=[]
with source.open('rb') as f:
    while b:=f.read(65536):
        if b.strip(b'\0'):base.append((f.tell()-len(b),b))
def reset():
    with source.open('wb') as f:
        f.truncate(size)
        for at,b in base:f.seek(at);f.write(b)
def digest_range(lo,hi):
    h=hashlib.sha256()
    with source.open('rb') as f:
        f.seek(lo)
        while hi>lo:
            b=f.read(min(hi-lo,65536));assert b;h.update(b);lo+=len(b)
    return h.digest()
ranges=[(0,first*512),((first+total)*512,journal*512),((journal+49)*512,size)]
outside=[digest_range(a,b) for a,b in ranges]
def external(check_namespace=True):
    assert [digest_range(a,b) for a,b in ranges]==outside,'write escaped FAT + WAL (includes MBR/GPT/owner/gap)'
    part=smoke.WORK/'check.img'
    with part.open('wb') as dst,source.open('rb') as src:
        dst.truncate(total*512);src.seek(first*512)
        for at in range(0,total*512,65536):
            b=src.read(min(65536,total*512-at))
            if b.strip(b'\0'):dst.seek(at);dst.write(b)
    p=subprocess.run(['/sbin/fsck.fat','-n',str(part)],capture_output=True,text=True)
    assert p.returncode==0,(p.stdout,p.stderr)
    if not check_namespace:return
    out=smoke.WORK/'export'
    if out.exists():shutil.rmtree(out)
    out.mkdir();subprocess.run(['mcopy','-s','-i',str(part),'::*',str(out)],check=True,capture_output=True)
    return {p.relative_to(out).as_posix():(None if p.is_dir() else hashlib.sha256(p.read_bytes()).digest()) for p in out.rglob('*')}
old=external()
def args(readonly=False,config=None):
    filename=f'blkdebug:{config}:{source}' if config else str(source)
    a=['-drive',f'if=none,id=flash,format=raw,file={filename},readonly={"on" if readonly else "off"}']
    if CONTROLLER=='uhci':a+=['-device','piix3-usb-uhci,id=uhci']
    else:a+=['-device','ich9-usb-ehci1,id=ehci','-device','pci-ohci,id=ohci1,masterbus=ehci.0,firstport=0','-device','pci-ohci,id=ohci2,masterbus=ehci.0,firstport=3']
    return a+['-device',device()]
def device():return 'usb-storage,id=flashdev,bus='+('uhci.0' if CONTROLLER=='uhci' else 'ehci.0')+',drive=flash,port='+('1' if CONTROLLER=='uhci' else '3')
counter=0
def log(q,label):
    global counter
    counter+=1;(smoke.WORK/f'case-{counter:03}-{label}.log').write_text(q.log.read_text(errors='replace'))
def command(q,mode='verify',cut=0):
    # Reset stage ourselves through a completed preceding command? The guest
    # resets it on entry. Await its new result line, not a stale stage=3.
    before=q.log.read_text(errors='replace').count('M2FAT result=')
    q.text(f'fattest {mode} {cut}\n')
    deadline=time.monotonic()+60
    if cut:
        target=abs(cut)
        while time.monotonic()<deadline:
            if q.num('fat_lab_stage')==2 and q.num('stx_lab_step')==target:return None
            if q.log.read_text(errors='replace').count('M2FAT result=')>before:raise AssertionError('pause point not reached')
            time.sleep(.01)
        raise AssertionError(('pause timeout',mode,target,q.log.read_text(errors='replace')[-1000:]))
    while time.monotonic()<deadline:
        if q.log.read_text(errors='replace').count('M2FAT result=')>before and q.num('fat_lab_stage')==3:
            return struct.unpack('<i',q.mem('fat_lab_result',4))[0]
        time.sleep(.02)
    raise AssertionError(('operation timeout',mode,q.log.read_text(errors='replace')[-1200:]))
def guest(**kw):return smoke.Guest(False,extra_args=args(**kw))
def cold(want=(1000,1300,0)):
    with guest() as q:
        assert 'recovery complete BEFORE namespace publication' in q.log.read_text(errors='replace')
        r=command(q);assert r in want,r;log(q,'cold')
    return external()
print('FAT USB/WAL:',CONTROLLER,LAYOUT,'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
if os.environ.get('FAT_LAB_GENERATION_ONLY'):
    with guest() as q:
        assert command(q,'stale')==-1
        assert q.num('stx_lab_step')==0,'old mount reached new-generation metadata writes'
        q.text('rescan\n');assert command(q)==1000;log(q,'fresh-grant-generation')
    assert external()==old
    print('PASS old mount refused despite NEW validated transport grant, fsck unchanged',flush=True)
    raise SystemExit
with guest() as q:
    assert command(q,'guard')==0;log(q,'range-guard')
assert external()==old
# Successful real-driver operations, then another cold VM before external fsck.
replug_only=bool(os.environ.get('FAT_LAB_REPLUG_ONLY'))
modes=[('overwrite',1300),('empty',0),('create',1300),('mkdir',0),('remove',0),('rmdir',0),('large',65535)]
for mode,want in (modes[:1] if replug_only else modes):
    reset()
    with guest() as q:
        r=command(q,mode);assert r==want,(mode,r,q.log.read_text(errors='replace'))
        if mode=='overwrite':
            n=q.num('stx_lab_step');assert n<=256
            trace=list(struct.iter_unpack('<II',q.mem('stx_lab_events',n*8)))
        log(q,mode)
    expected=dict(old)
    if mode in ('overwrite','empty'):expected['Old long name.txt']=hashlib.sha256(body[:want]).digest()
    elif mode=='create':expected['Новый длинный файл.txt']=hashlib.sha256(body[:1300]).digest()
    elif mode=='mkdir':expected['New long directory']=None
    elif mode=='remove':del expected['Old long name.txt']
    elif mode=='rmdir':del expected['Empty long directory']
    else:expected['Large.txt']=hashlib.sha256(body).digest()
    with guest() as q:log(q,'reopen-'+mode)
    assert external()==expected,(mode,'namespace/content differs')
    if mode=='overwrite':new=expected
    print('  VFS',mode,'+ cold guest + external namespace/fsck PASS',flush=True)
# Select actual protocol boundaries by LBA from the trace, not guessed timers.
reserved=struct.unpack_from('<H',bpb,14)[0];fatsz=struct.unpack_from('<I',bpb,36)[0]
fsi=struct.unpack_from('<H',bpb,48)[0];backup=struct.unpack_from('<H',bpb,50)[0]
root=first+reserved+2*fatsz
cuts=[]
for label,lba in [('FAT1',first+reserved),('FAT2',first+reserved+fatsz),('FSInfo',first+fsi),('backup-FSInfo',first+backup+fsi),('directory',root)]:
    index=next(i+1 for i,(phase,at) in enumerate(trace) if phase==2 and at==lba)
    cuts.append((label,index))
last_header=max(i+1 for i,(phase,at) in enumerate(trace) if phase==1 and at==journal)
cuts += [('before-close',last_header),('after-close',last_header+1),('after-close-barrier',len(trace))]
if os.environ.get('FAT_LAB_QUICK'):cuts=cuts[:1]
for label,cut in ([] if replug_only else cuts):
    reset()
    with guest() as q:
        command(q,'overwrite',cut);log(q,'crash-'+label)
        assert 'M2FAT result=' not in q.log.read_text(errors='replace'),'interrupted operation claimed completion'
        q.proc.kill();q.proc.wait(timeout=5) # no orderly guest/QEMU shutdown
    assert cold() in (old,new),(label,'not a complete old/new filesystem')
    print('  Crash at',label,'-> recovery before publication + cold guest/fsck PASS',flush=True)
if os.environ.get('FAT_LAB_QUICK'):raise SystemExit
# A backend failure is not reported as a successful file save; journal stays
# available to a fresh generation, because all grants are revoked on I/O error.
for label,event,lba in ([] if replug_only else [('FAT2-EIO','write_aio',first+reserved+fatsz),('directory-EIO','write_aio',root),('flush-EIO','flush_to_disk',None)]):
    reset();config=smoke.WORK/(label+'.conf')
    config.write_text(f'[inject-error]\nevent = "{event}"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n'+(f'sector = "{lba}"\n' if lba is not None else ''))
    with guest(config=config) as q:
        assert command(q,'overwrite')<0;log(q,label)
        assert 'rw=0' in q.log.read_text(errors='replace')
    assert cold() in (old,new)
    print(' ',label,'-> error/fence + reconnect recovery/fsck PASS',flush=True)
if not replug_only:
    reset()
    with guest(readonly=True) as q:assert command(q,'overwrite')<0;log(q,'readonly')
    assert cold()==old
    print('  Read-only SCSI device -> failure, original filesystem intact PASS',flush=True)
# Actual device_del while the first FAT copy has changed and WAL is durable.
reset()
with guest() as q:
    command(q,'overwrite',-cuts[0][1]);q.cmd('device_del flashdev')
    until=time.monotonic()+15
    while time.monotonic()<until and q.num('fat_lab_stage')!=3:time.sleep(.02)
    assert q.num('fat_lab_stage')==3 and struct.unpack('<i',q.mem('fat_lab_result',4))[0]<0
    assert 'rw=0' in q.log.read_text(errors='replace');log(q,'unplug')
    # Complete the offline scan/port-change acknowledgement before replug.
    # QEMU also auto-deletes the legacy -drive backend on device_del.
    q.text('rescan\n');assert command(q)==-2;assert q.num('disk_count')==0
    reply=q.cmd(f'drive_add 0 if=none,id=flash,file={source},format=raw');assert b'Error:' not in reply,reply
    reply=q.cmd('device_add '+device());assert b'Error:' not in reply,reply
    q.text('rescan\n');r=command(q);assert r==1000,(r,q.log.read_text(errors='replace')[-1500:]);log(q,'replug-recovery')
assert external()==old
print('  Actual USB unplug mid-write -> bounded error, same-VM replug/rescan recovery + fsck PASS',flush=True)
if replug_only:raise SystemExit
reset()
with guest() as q:
    assert command(q,'stale')==-1;q.text('rescan\n');assert command(q)==1000;log(q,'generation')
assert external()==old
print('  Stale mount generation cannot reuse new transport grants; rescan recovers before RW PASS',flush=True)
# Fail DURING replay: no volume may be registered on a partial undo.
for label,event,lba in [('recovery-write-EIO','write_aio',first+reserved+fatsz),('recovery-read-EIO','read_aio',journal+1)]:
    reset()
    with guest() as q:
        command(q,'overwrite',dict(cuts)['directory']);q.proc.kill();q.proc.wait(timeout=5)
    before=digest_range(0,size)
    config=smoke.WORK/(label+'.conf')
    config.write_text(f'[inject-error]\nevent = "{event}"\nsector = "{lba}"\nerrno = "5"\nonce = "on"\nimmediately = "on"\n')
    with guest(config=config) as q:
        assert command(q)==-2;q.text('mount\n');assert ('/mnt/'+vol) not in q.log.read_text(errors='replace');log(q,label)
    if event=='read_aio':assert digest_range(0,size)==before
    assert cold()==old
    print(' ',label,'-> namespace withheld; next cold replay completes + fsck PASS',flush=True)
# A fully exhausted FAT partition must not free the old file to make room.
reset()
with source.open('rb') as f:f.seek((first+fsi)*512+488);free=struct.unpack('<I',f.read(4))[0]
filler=smoke.WORK/'fill.bin'
with filler.open('wb') as f:f.truncate(free*512)
subprocess.run(['mcopy','-i',str(source)+'@@'+str(first*512),str(filler),'::FILL.BIN'],check=True,capture_output=True)
external(False);before=digest_range(first*512,(first+total)*512)
with guest() as q:assert command(q,'overwrite')==-3;assert command(q)==1000;log(q,'full')
assert digest_range(first*512,(first+total)*512)==before;external(False);filler.unlink()
print('  Completely full USB FAT32 -> refusal, entire FAT partition byte-identical + fsck PASS',flush=True)
# Invalid recognized manifest/header: no recovery writes, no namespace exposure.
for label,at in [('manifest-CRC',(journal-1)*512+100),('journal-CRC',journal*512+100)]:
    reset()
    with source.open('r+b') as f:f.seek(at);f.write(b'X')
    before=digest_range(0,size)
    with guest() as q:
        assert command(q)==-2;q.text('mount\n');assert ('/mnt/'+vol) not in q.log.read_text(errors='replace');log(q,label)
    assert digest_range(0,size)==before
    print(' ',label,'-> namespace withheld, entire image unchanged PASS',flush=True)
# No owner marker: regular USB remains RO, even in a lab build.
reset()
with source.open('r+b') as f:f.seek((journal-1)*512);f.write(b'\0'*512)
before=digest_range(0,size)
with guest() as q:assert command(q)==-2;log(q,'unmarked')
assert digest_range(0,size)==before
print('  Unmarked image stays RO; no automatic journal provisioning PASS',flush=True)

reset()
with guest() as q:
    command(q,'overwrite',dict(cuts)['directory']);q.proc.kill();q.proc.wait(timeout=5)
with source.open('r+b') as f:f.seek((journal+1)*512+100);b=f.read(1);f.seek(-1,1);f.write(bytes([b[0]^1]))
before=digest_range(0,size)
with guest() as q:assert command(q)==-2;q.text('mount\n');assert ('/mnt/'+vol) not in q.log.read_text(errors='replace');log(q,'backup-CRC')
assert digest_range(0,size)==before
print('  Corrupt backup -> no guessed rollback, namespace withheld, image unchanged PASS',flush=True)
# Restore an intact owned fixture for any subsequent manual test.
reset()
print('PASS USB FAT/WAL end-to-end matrix complete',flush=True)
