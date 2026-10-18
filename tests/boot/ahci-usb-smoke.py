#!/usr/bin/env python3
"""Mixed SATA/USB ordinary owned-WAL RW + default-RO regression, both architectures.
Uses only disposable images; this is not a new exhaustive M2 crash matrix.
"""
from pathlib import Path
import hashlib,os,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];os.chdir(ROOT)
if 'MCC_ARCH64' not in os.environ:
    for arch in ('0','1'):
        subprocess.run([sys.executable,__file__],env=dict(os.environ,MCC_ARCH64=arch),check=True)
    raise SystemExit
sys.path.insert(0,str(ROOT/'tests/mcc'));import smoke
from mkfatowned import make
smoke.WORK=ROOT/'build/ahci-usb'/('uefi64' if smoke.IS64 else 'bios32');smoke.WORK.mkdir(parents=True,exist_ok=True)
sata,sata_first,sata_total,sata_journal,_=make(smoke.WORK/'sata','mbr')
def digest(p,start=0,length=None):
    h=hashlib.sha256()
    with p.open('rb') as f:
        f.seek(start)
        while length is None or length:
            b=f.read(65536 if length is None else min(length,65536))
            if not b:break
            h.update(b)
            if length is not None:length-=len(b)
    return h.digest()
sata_before=digest(sata)
sata_outside=[digest(sata,0,sata_first*512),digest(sata,(sata_first+sata_total)*512),digest(sata,(sata_journal-1)*512,512)]
def command(q,line):
    at=len(q.log.read_bytes());q.text(line+'\n');end=time.monotonic()+30
    while time.monotonic()<end:
        out=q.log.read_bytes()[at:]
        if b'kvant:/$ ' in out:return out
        time.sleep(.03)
    raise AssertionError(('mixed-bus command timeout',line))
print('Mixed AHCI/USB normal guest:', 'UEFI64' if smoke.IS64 else 'BIOS32',flush=True)
controllers=(('ehci-ohci','gpt'),('uhci','mbr'),('ohci','gpt'))
selected=os.environ.get('USB_CONTROLLER')
assert selected is None or selected in {c for c,_ in controllers}
for controller,layout in controllers:
    if selected and controller!=selected:continue
    image,first,total,journal,_=make(smoke.WORK/controller,layout)
    volume='usb0p2' if layout=='gpt' else 'usb0p1';root='/mnt/'+volume
    outside=[digest(image,0,first*512),digest(image,(first+total)*512),digest(image,(journal-1)*512,512)]
    args=['-device','ich9-ahci,id=satactl','-drive',f'if=none,id=satadrive,format=raw,file={sata}',
          '-device','ide-hd,bus=satactl.0,drive=satadrive',
          '-drive',f'if=none,id=flash,format=raw,file={image}']
    if controller=='uhci':args+=['-device','piix3-usb-uhci,id=usbtest'];port='1'
    elif controller=='ohci':args+=['-device','pci-ohci,id=usbtest'];port='1'
    else:
        args+=['-device','ich9-usb-ehci1,id=usbtest',
               '-device','pci-ohci,id=comp0,masterbus=usbtest.0,firstport=0',
               '-device','pci-ohci,id=comp1,masterbus=usbtest.0,firstport=3'];port='3'
    args+=['-device','usb-storage,bus=usbtest.0,drive=flash,port='+port]
    with smoke.Guest(disk=False,extra_args=args) as q:
        assert q.num('ahci_drive_count')==1
        out=command(q,'mount');assert b'/mnt/sata0p1' in out and root.encode() in out,out
        assert b'RW enabled for this session' in command(q,'fatwrite sata0p1 enable')
        assert b'FAT32 RO' in command(q,'fatwrite sata0p1 disable')
        assert b'RW enabled for this session' in command(q,'fatwrite '+volume+' enable')
        assert b'Copied' in command(q,f'vcp {root}/F00.TXT /mnt/ram/seed.txt')
        assert q.files()['seed.txt']==b'O'*1000
        for target in ('NEW.TXT','F01.TXT'):
            out=command(q,f'vcp /mnt/ram/seed.txt {root}/{target}');assert b'Copied' in out,out
        assert b'Copied' not in command(q,'vcp /mnt/ram/seed.txt /mnt/sata0p1/NO.TXT')
        assert digest(sata)==sata_before,'USB writes changed RO SATA image'
        assert b'RW enabled for this session' in command(q,'fatwrite sata0p1 enable')
        out=command(q,'vcp /mnt/ram/seed.txt /mnt/sata0p1/SNEW.TXT');assert b'Copied' in out,out
        # Both volumes RW in the SAME guest: leases must not cross transports.
        out=command(q,f'vcp /mnt/ram/seed.txt {root}/BOTH.TXT');assert b'Copied' in out,out
        command(q,'rescan');out=command(q,'mount')
        assert all(b' ro' in line for line in out.splitlines() if b'/mnt/sata' in line or root.encode() in line),out
        (smoke.WORK/(controller+'-write.serial.log')).write_bytes(q.log.read_bytes())
    with smoke.Guest(disk=False,extra_args=args) as q:
        out=command(q,f'vcp {root}/NEW.TXT /mnt/ram/cold.txt');assert b'Copied' in out,out
        assert q.files()['cold.txt']==b'O'*1000
        out=command(q,'vcp /mnt/sata0p1/SNEW.TXT /mnt/ram/sata.txt');assert b'Copied' in out,out
        assert q.files()['sata.txt']==b'O'*1000
        at=len(q.log.read_bytes());q.cmd('system_reset');deadline=time.monotonic()+60
        while b'kvant:/$ ' not in q.log.read_bytes()[at:]:
            assert time.monotonic()<deadline,'warm reset did not reach shell'
            time.sleep(.1)
        out=command(q,'mount')
        assert all(b' ro' in line for line in out.splitlines() if b'/mnt/sata' in line or root.encode() in line),out
        out=command(q,'vcp /mnt/sata0p1/SNEW.TXT /mnt/ram/warm.txt');assert b'Copied' in out,out
        assert q.files()['warm.txt']==b'O'*1000
        (smoke.WORK/(controller+'-cold.serial.log')).write_bytes(q.log.read_bytes())
    assert sata_outside==[digest(sata,0,sata_first*512),digest(sata,(sata_first+sata_total)*512),digest(sata,(sata_journal-1)*512,512)]
    sata_before=digest(sata)
    assert outside==[digest(image,0,first*512),digest(image,(first+total)*512),digest(image,(journal-1)*512,512)]
    part=smoke.WORK/controller/'external-check.img'
    with image.open('rb') as src,part.open('wb') as dst:
        src.seek(first*512);dst.truncate(total*512)
        for offset in range(0,total*512,65536):
            b=src.read(min(65536,total*512-offset))
            if b.strip(b'\0'):dst.seek(offset);dst.write(b)
    subprocess.run(['/sbin/fsck.fat','-n',str(part)],check=True,capture_output=True)
    body=subprocess.check_output(['mtype','-i',str(part),'::NEW.TXT']);assert body==b'O'*1000
    sata_part=smoke.WORK/controller/'sata-check.img'
    with sata.open('rb') as src,sata_part.open('wb') as dst:
        src.seek(sata_first*512);dst.truncate(sata_total*512)
        for offset in range(0,sata_total*512,65536):
            b=src.read(min(65536,sata_total*512-offset))
            if b.strip(b'\0'):dst.seek(offset);dst.write(b)
    subprocess.run(['/sbin/fsck.fat','-n',str(sata_part)],check=True,capture_output=True)
    assert subprocess.check_output(['mtype','-i',str(sata_part),'::SNEW.TXT'])==b'O'*1000
    print(f'  {controller}/{layout}: independent USB + SATA WAL create/overwrite/cold + warm reads, both fsck; initial RO + rescan RO; GPT/gaps/owners unchanged PASS',flush=True)
print('PASS mixed-bus ordinary policy, stable USB/SATA routing, no lab hooks',flush=True)
