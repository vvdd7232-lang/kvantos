"""Read-only decoder for the NORMAL kernel's 96-byte EHCI last-fault record."""
import struct
REASONS={0:'none',1:'link',2:'tsc-budget',3:'250-frames',4:'iteration-budget',5:'ASS-retirement',
         6:'qTD/QH-error',7:'ambiguous-split',8:'writeback',9:'short-OUT',10:'DMA-address',11:'ASS-start'}
FIELDS=('start_tsc','stop_tsc','reason','retired','iterations','start_frame','stop_frame','pre_td','pre_overlay',
        'post_td','post_overlay','command','status','port','current','next','endpoint','caps','address','generation','len','post_frame')
def faults(q):
    raw=q.mem('ehci_faults',4*96);result=[]
    for pool in range(4):
        d=dict(zip(FIELDS,struct.unpack_from('<QQ20I',raw,pool*96)))
        if not d['reason']:continue
        d.update(pool=pool,reason_name=REASONS.get(d['reason'],'unknown'),tsc_ticks=d['stop_tsc']-d['start_tsc'],
                 frame_delta=(d['stop_frame']-d['start_frame'])&2047,
                 pre_active=bool(d['pre_td']&128),pre_split=bool(d['pre_overlay']&2))
        result.append(d)
    return result

def dump_state(q,directory):
    import subprocess
    wanted={'ehci_faults','ehci_pool','hcs','disks','fat_tx','fat_vols'}
    records=[]
    for line in subprocess.check_output(['nm','-S','build/kvant64.bin' if __import__('smoke').IS64 else 'build/kvant.bin'],text=True).splitlines():
        m=line.split()
        if len(m)==4 and m[3] in wanted:
            size=int(m[1],16);assert size<=65536
            p=directory/(m[3]+'.bin');p.write_bytes(q.mem(m[3],size))
            records.append(dict(symbol=m[3],address=int(m[0],16),bytes=size,file=p.name))
    return records
