#!/usr/bin/env python3
"""Inspect/provision an owned WAL file on FAT32. NEVER formats/repartitions.
Default is read-only inspection. Preparation is explicit and uses mtools to
allocate a normal file, followed by bounded writes to that proven-owned file.
On Linux, physical targets must be UNMOUNTED PARTITION block devices, not disks.
An exclusive-open + mount/swap/dependency check is required. Image offsets are
supported ONLY for regular files. No recovery, journal reset or existing-file
replacement is implemented: pending logs belong to KvantOS, not fsck repair.
"""
from pathlib import Path
from collections import deque
from functools import lru_cache
import argparse, contextlib, fcntl, os, shutil, stat, struct, subprocess, tempfile, zlib

MAGIC=b'KVANTFATWAL1\0\0\0\0'
NAME=b'KVANTWALSYS'
WORKDIR=None
VIRTUAL=0xFFFFFE00
U32=lambda b,o: struct.unpack_from('<I',b,o)[0]
U16=lambda b,o: struct.unpack_from('<H',b,o)[0]

def need(test,message):
    if not test: raise ValueError(message)

def run(*args):
    p=subprocess.run([str(a) for a in args],stdin=subprocess.DEVNULL,capture_output=True,text=True)
    need(p.returncode==0, f'{args[0]} failed: {p.stdout} {p.stderr}')
    return p.stdout

class Volume:
    def __init__(self,fd,start,capacity):
        self.fd,self.start,self.capacity=fd,start,capacity
        self.bpb=self.read(0)
        b=self.bpb
        need(b[510:512]==b'\x55\xaa' and U16(b,11)==512,'512-byte FAT boot sector required')
        self.spc=b[13];self.reserved=U16(b,14);self.nf=b[16]
        self.total=U32(b,32);self.fatsz=U32(b,36);self.root=U32(b,44);self.serial=U32(b,67)
        need(self.spc in (1,2,4,8,16,32,64,128) and self.reserved and self.nf==2,
             'FAT32 with two mirrored FATs required')
        need(U16(b,17)==0 and U16(b,19)==0 and U16(b,22)==0 and U16(b,40)==0 and U16(b,42)==0,
             'standard mirrored FAT32 only')
        self.data=self.reserved+2*self.fatsz
        self.clusters=(self.total-self.data)//self.spc
        need(self.fatsz and self.data<self.total<VIRTUAL and 65525<=self.clusters<0x0FFFFFEE and
             self.fatsz*128>=self.clusters+2 and 2<=self.root<self.clusters+2 and
             0<=start and self.total<=capacity-start,'invalid FAT32 extent/geometry')
        fsi=U16(b,48);backup=U16(b,50)
        need(0<fsi<self.reserved and (backup in (0,65535) or backup+fsi<self.reserved),'FSInfo outside reserved area')
    def read(self,sec,count=1):
        need(sec>=0 and count>0 and self.start+sec+count<=self.capacity,'read outside target')
        b=os.pread(self.fd,count*512,(self.start+sec)*512)
        need(len(b)==count*512,'short read')
        return b
    def sector(self,c): return self.data+(c-2)*self.spc
    @lru_cache(maxsize=64)
    def fat_sector(self,s): return self.read(self.reserved+s)
    def fat(self,c):
        need(2<=c<self.clusters+2,'bad cluster')
        return U32(self.fat_sector(c//128),(c%128)*4)&0x0FFFFFFF
    def chain(self,c):
        seen=set()
        while c<0x0FFFFFF8:
            need(2<=c<self.clusters+2 and c not in seen,'invalid/cyclic chain')
            seen.add(c);yield c;c=self.fat(c)
    def entries(self,c):
        chain=list(self.chain(c)) # validate even the suffix after a 00 entry
        for c in chain:
            for j in range(self.spc):
                sec=self.sector(c)+j;b=self.read(sec)
                for off in range(0,512,32):
                    d=b[off:off+32]
                    if not d[0]: return
                    if d[0]==229 or d[11]&15==15 or d[11]&8: continue
                    yield sec,off,d
    def owner(self):
        rows=[r for r in self.entries(self.root) if r[2][:11]==NAME]
        need(len(rows)<=1,'duplicate WAL filename')
        if not rows: return None
        sec,off,d=rows[0];c=(U16(d,20)<<16)|U16(d,26)
        clusters=(50+self.spc-1)//self.spc;sectors=clusters*self.spc
        need(d[11]&0xDF==7 and U32(d,28)==sectors*512 and list(self.chain(c))==list(range(c,c+clusters)),
             'WAL file must be contiguous, exact size, read-only + hidden + system')
        need(not c<=self.root<c+clusters,'WAL overlaps BPB root directory')
        fields=(1,zlib.crc32(self.bpb),self.serial,self.total,self.spc,c,clusters,sectors,
                sec,off,VIRTUAL,49,self.root,self.data,self.nf,self.fatsz)
        return self.sector(c),fields,rows[0]
    def check_tree(self,owner=None):
        queue=deque([self.root]);seen={self.root};entries=found=0
        while queue:
            parent=queue.popleft()
            for sec,off,d in self.entries(parent):
                entries+=1;need(entries<=262144,'namespace entry budget exceeded')
                c=(U16(d,20)<<16)|U16(d,26)
                if owner:
                    _,fields,row=owner;first,number=fields[5:7]
                    if (sec,off)==row[:2]:
                        need(parent==self.root and d==row[2],'owner entry mismatch');found+=1;continue
                    need(not first<=c<first+number,'directory alias into WAL file')
                    need(not(parent==self.root and d[:11]==NAME),'duplicate WAL filename')
                if d[:11] in (b'.          ',b'..         '): continue
                if d[11]&16:
                    need(2<=c<self.clusters+2 and c not in seen and len(seen)<1024,'invalid/shared directory or directory budget exceeded')
                    seen.add(c);queue.append(c)
        if owner: need(found==1,'WAL owner is not unique')
    def check_fats(self,owner=None):
        for s in range(self.fatsz):
            a=self.read(self.reserved+s);b=self.read(self.reserved+self.fatsz+s)
            for off in range(0,512,4):
                n=U32(a,off)&0x0FFFFFFF;i=s*128+off//4
                need(n==(U32(b,off)&0x0FFFFFFF),'FAT mirrors differ (no repair attempted)')
                if i==1: need(n==0x0FFFFFFF,'dirty/error FAT: external check required BEFORE enrollment')
                if not owner or not 2<=i<self.clusters+2: continue
                _,fields,_=owner;first,number=fields[5:7]
                if first<=i<first+number:
                    need(n>=0x0FFFFFF8 if i==first+number-1 else n==i+1,'WAL chain changed')
                if first<=n<first+number:
                    need(first<=i<first+number-1 and n==i+1,'foreign/lost FAT chain enters WAL file')
    def inspect_owner(self,owner):
        sec,fields,_=owner;h=self.read(sec);log=self.read(sec+1)
        need(h[:16]==MAGIC and U32(h,508)==zlib.crc32(h[:508]) and
             struct.unpack_from('<16I',h,64)==fields,'owner binding/CRC mismatch; do NOT reset')
        need(log[:8]==b'KVSTX01\0' and U32(log,508)==zlib.crc32(log[:508]) and
             struct.unpack_from('<6I',log,8)==(1,self.serial,0,self.total,VIRTUAL,49),
             'WAL header mismatch/corrupt; do NOT reset')
        active,n=struct.unpack_from('<2I',log,32)
        need(active in (0,1) and n<=48 and (active or n==0),'invalid WAL state')
        targets=set()
        for i in range(n):
            at,crc=struct.unpack_from('<2I',log,40+8*i)
            need(at<self.total and not sec<=at<sec+fields[7] and at not in targets and
                 zlib.crc32(self.read(sec+2+i))==crc,'invalid undo record; do NOT reset')
            targets.add(at)
        if active: print(f'PENDING ({n} undo records): boot KvantOS to recover FIRST. No other OS writes/fsck repair!')
        else:
            print('WAL header is CLEAN; validating ownership (no writes).')
            self.check_fats(owner);self.check_tree(owner);print('READY, WAL clean, ownership validated; KvantOS still mounts RO until explicit enable.')
        return active

@contextlib.contextmanager
def filesystem_path(path,volume):
    # fsck.fat has no portable offset option. Private regular-file slice only.
    with tempfile.TemporaryDirectory(prefix='kvant-fat-check-',dir=WORKDIR) as t:
        p=Path(t)/'partition.img'
        with p.open('wb') as f:
            f.truncate(volume.total*512)
            for s in range(0,volume.total,2048):
                b=volume.read(s,min(2048,volume.total-s))
                if b.strip(b'\0'): f.seek(s*512);f.write(b)
        yield p

def external_check(path,v):
    with filesystem_path(path,v) as check:
        run(shutil.which('fsck.fat') or '/sbin/fsck.fat','-n',check)

def block_dependencies(mm):
    p=(Path('/sys/dev/block')/mm).resolve(strict=True)
    out={p}
    for child in (p/'slaves').glob('*'): out.update(block_dependencies((child/'dev').read_text().strip()))
    return out

def unmounted_partition(st):
    mm=f'{os.major(st.st_rdev)}:{os.minor(st.st_rdev)}';p=(Path('/sys/dev/block')/mm).resolve(strict=True)
    need((p/'partition').is_file(),'physical target must be a PARTITION, never a whole disk')
    busy=[line.split()[2] for line in Path('/proc/self/mountinfo').read_text().splitlines()]
    for row in Path('/proc/swaps').read_text().splitlines()[1:]:
        dev=os.stat(row.split()[0])
        if stat.S_ISBLK(dev.st_mode): busy.append(f'{os.major(dev.st_rdev)}:{os.minor(dev.st_rdev)}')
    for m in busy:
        if not (Path('/sys/dev/block')/m).exists(): continue
        for dep in block_dependencies(m):
            need(not(p==dep or p in dep.parents or dep in p.parents),'target/parent/dependent device is mounted or swap')

def main():
    global WORKDIR
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('target',type=Path);ap.add_argument('--start',type=int,default=0,help='FAT offset in 512-byte sectors; regular images ONLY')
    ap.add_argument('--temporary-directory',type=Path,help='existing directory with room for two sparse partition copies; useful for physical preparation')
    ap.add_argument('--prepare',action='store_true',help='explicitly allocate a new WAL file (requires backup, exclusive access, uninterrupted host preparation)')
    ap.add_argument('--confirm',help='exact volume serial, 8 hex digits as printed by read-only inspection')
    ap.add_argument('--allow-block-partition',action='store_true',help='Linux only; opt in to an unmounted partition device; never a whole disk')
    a=ap.parse_args();WORKDIR=a.temporary_directory
    need(WORKDIR is None or WORKDIR.is_dir(),'temporary directory does not exist')
    path=a.target.resolve(strict=True);st=path.stat();block=stat.S_ISBLK(st.st_mode)
    need(stat.S_ISREG(st.st_mode) or block,'only regular images or explicit partition block devices')
    if block:
        need(a.allow_block_partition and a.start==0,'physical access needs --allow-block-partition and --start 0')
        unmounted_partition(st)
    fd=os.open(path,(os.O_RDWR if a.prepare else os.O_RDONLY)|(os.O_EXCL if block else 0))
    try:
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        now=os.fstat(fd);need((now.st_dev,now.st_ino,now.st_rdev)==(st.st_dev,st.st_ino,st.st_rdev),'target changed while opening')
        if block:
            need(struct.unpack('I',fcntl.ioctl(fd,0x1268,bytes(4)))[0]==512,'device logical sectors must be 512 bytes')
            capacity=struct.unpack('Q',fcntl.ioctl(fd,0x80081272,bytes(8)))[0]//512
            unmounted_partition(st)
        else:
            need(now.st_size%512==0,'image size not sector aligned');capacity=now.st_size//512
        v=Volume(fd,a.start,capacity)
        print(f'FAT32 serial={v.serial:08X}, sectors={v.total}, cluster={v.spc*512}B, target={path}, start={a.start}')
        owner=v.owner()
        if owner:
            v.inspect_owner(owner)
            need(not a.prepare,'existing file is NEVER overwritten, reset or reinitialized')
            return
        print('No owned WAL. KvantOS will not enable FAT writes.')
        if not a.prepare: return
        need(a.confirm==f'{v.serial:08X}','--confirm must exactly match the printed serial')
        v.check_fats();v.check_tree()
        # mtools NEVER reopens the user's target path. Apart from ordinary path
        # races, USB unplug/reconnect can reuse /dev/sdX for a DIFFERENT stick.
        # Stage the standard file operation on private copies, validate, compare
        # the ORIGINAL fd against the snapshot, then apply bounded deltas to it.
        with tempfile.TemporaryDirectory(prefix='kvant-fat-stage-',dir=WORKDIR) as t:
            original=Path(t)/'original.img';staged=Path(t)/'staged.img'
            with original.open('wb') as f:
                f.truncate(v.total*512)
                for at in range(0,v.total,2048):
                    chunk=v.read(at,min(2048,v.total-at))
                    if chunk.strip(b'\0'):f.seek(at*512);f.write(chunk)
            # Reflink where available; sparse fallback avoids materializing free
            # zero space. Physical preparation needs sufficient temporary space.
            run('cp','--reflink=auto','--sparse=always',original,staged)
            run(shutil.which('fsck.fat') or '/sbin/fsck.fat','-n',staged)
            location=str(staged)
            probe=subprocess.run(['mdir','-i',location,'::KVANTWAL.SYS'],stdin=subprocess.DEVNULL,capture_output=True)
            need(probe.returncode!=0,'reserved name/long-name alias already exists; no overwrite')
            blank=Path(t)/'blank';blank.write_bytes(bytes(((50+v.spc-1)//v.spc)*v.spc*512))
            run('mcopy','-D','sS','-i',location,blank,'::KVANTWAL.SYS')
            run('mattrib','-i',location,'+r','+h','+s','::KVANTWAL.SYS')
            stagefd=os.open(staged,os.O_RDONLY)
            try:
                sv=Volume(stagefd,0,v.total);need(sv.bpb==v.bpb,'staging changed BPB; no writes authorized')
                planned=sv.owner();need(planned is not None,'reserved file not created')
                sv.check_fats(planned);sv.check_tree(planned)
                need(not sv.read(planned[0],planned[1][7]).strip(b'\0'),'new WAL file not zeroed')
                run(shutil.which('fsck.fat') or '/sbin/fsck.fat','-n',staged)
            finally:os.close(stagefd)
            changes=[]
            with original.open('rb') as before,staged.open('rb') as after:
                for at in range(0,v.total,2048):
                    count=min(2048,v.total-at);old=before.read(count*512);new=after.read(count*512)
                    need(old==v.read(at,count),'original target changed; NO writes authorized')
                    for i in range(count):
                        x,y=old[i*512:(i+1)*512],new[i*512:(i+1)*512]
                        if x!=y:
                            changes.append((at+i,x,y));need(len(changes)<=4096,'unexpectedly large host filesystem change')
            # Fresh data/root-extension bodies before metadata, existing root
            # publication last. Bootstrap still requires uninterrupted host
            # preparation: no writable KvantOS ownership exists until the LAST
            # owner-sector commit below. A partial bootstrap needs external
            # fsck while unready, not guessed journal recovery.
            origfd=os.open(original,os.O_RDONLY)
            try:
                ov=Volume(origfd,0,v.total)
                def phase(change):
                    sec=change[0]
                    if sec>=v.data:
                        c=(sec-v.data)//v.spc+2
                        if 2<=c<v.clusters+2 and ov.fat(c)==0: return 0
                        return 2
                    return 1
                changes.sort(key=phase)
            finally:os.close(origfd)
            last=-1
            for change in changes:
                phase_number=0 if change[0]>=v.data else 1
                if phase_number!=last:os.fsync(fd);last=phase_number
                sec,old,new=change
                need(v.read(0)==v.bpb and v.read(sec)==old,'target identity/content changed during preparation; stop, do not retry automatically')
                need(os.pwrite(fd,new,(a.start+sec)*512)==512,'short filesystem preparation write')
            os.fsync(fd)
        v=Volume(fd,a.start,capacity);owner=v.owner();need(owner is not None,'reserved file missing after host preparation')
        v.check_fats(owner);v.check_tree(owner)
        sec,fields,_=owner
        need(not v.read(sec,fields[7]).strip(b'\0'),'new WAL file was not zeroed; refusing to initialize')
        log=bytearray(512);log[:8]=b'KVSTX01\0'
        struct.pack_into('<8I',log,8,1,v.serial,0,v.total,VIRTUAL,49,0,0)
        struct.pack_into('<I',log,508,zlib.crc32(log[:508]))
        header=bytearray(512);header[:16]=MAGIC;struct.pack_into('<16I',header,64,*fields)
        struct.pack_into('<I',header,508,zlib.crc32(header[:508]))
        # Commit ownership LAST. A crash earlier leaves an invalid/unready file,
        # not permission to write. Kernel never initializes or guesses its state.
        need(os.pwrite(fd,log,(a.start+sec+1)*512)==512,'short WAL header write');os.fsync(fd)
        need(os.pwrite(fd,header,(a.start+sec)*512)==512,'short owner write');os.fsync(fd)
        v.inspect_owner(owner);external_check(path,v)
        print('Prepared. Backup + exclusive-media contract required. Never run fsck repair before KvantOS recovery.')
    finally: os.close(fd)

if __name__=='__main__':
    try: main()
    except (ValueError,OSError) as e: raise SystemExit(f'REFUSED: {e}')
