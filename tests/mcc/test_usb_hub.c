/* Actual production topology/descriptor code, simulated control bus only.
 * Global address lookup deliberately does NOT use the intended route: an
 * accidentally enabled old subtree must expose an address collision here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#define HC_MAX 4
#define USB_MAX 8
typedef struct { u8 kind,ports,dead; } hc_t;
typedef struct {
    hc_t *hc;
    u32 sectors,write_start,write_count,generation;
    u8 write_allowed,sense_key,asc,ascq;
    u16 in_mps,out_mps;
    u8 address,port,ep0,in,out,itoggle,otoggle,iface,parent,downport,route_dead,speed;
} usb_disk_t;
static hc_t hcs[HC_MAX];
static usb_disk_t disks[USB_MAX];
static int disk_count;
static u32 enumeration_generation;
static u8 config[1024],reply[64];
static int control(usb_disk_t *,u8,u8,u16,u16,void *,u16);
static int command(usb_disk_t *,const u8 *,u8,void *,u32);
static u32 port_status(hc_t *,int);
static int frames_wait(hc_t *,unsigned);
static void usb_stop(hc_t *h) { h->dead=1; }
static u32 be32(const u8 *p) { return p[3]|((u32)p[2]<<8)|((u32)p[1]<<16)|((u32)p[0]<<24); }
#include "../../kernel/usb_hub.h"

#define NODES 144
static struct {
    int hub,parent,port,root,address,attached,enabled,ports,queries;
    unsigned speed,protocol;
    u32 status[16],state;
} nodes[NODES];
static int count,root_on[15],fail_disable,reset_change,wait_fail,ports_override,desc_bad,config_bad,dev_bad,sets;
static int visible(int i) {
    if(!nodes[i].attached) return 0;
    int p=nodes[i].parent;
    return p<0?root_on[nodes[i].root]:visible(p) && (nodes[p].status[nodes[i].port]&2);
}
static int lookup(unsigned address) {
    int found=-1;
    for(int i=0;i<count;i++) if(visible(i) && nodes[i].address==(int)address) { assert(found<0);found=i; }
    assert(found>=0);return found;
}
static void address_unique(void) {
    for(int i=0;i<count;i++) if(visible(i)) for(int j=0;j<i;j++)
        assert(!visible(j) || nodes[j].address!=nodes[i].address);
}
static void reset_node(int i) {
    nodes[i].address=0;
    if(nodes[i].hub==1) for(int p=1;p<=nodes[i].ports;p++) {
        nodes[i].status[p]=0x100;
        for(int j=0;j<count;j++) if(nodes[j].parent==i && nodes[j].port==p && nodes[j].attached) nodes[i].status[p]|=0x10001;
    }
}
static int add(int parent,int port,int hub,int ports) {
    assert(count<NODES);int i=count++;
    nodes[i].parent=parent;nodes[i].port=port;nodes[i].root=parent<0?port:nodes[parent].root;
    nodes[i].hub=hub;nodes[i].ports=ports;nodes[i].speed=hcs[0].kind==1?0x400:0;nodes[i].protocol=hub==1 && hcs[0].kind==1?1:0;nodes[i].attached=1;return i;
}
static void output(void *b,unsigned len,u32 value) {
    assert(len==4);u8 *p=b;for(unsigned i=0;i<4;i++) p[i]=(u8)(value>>(8*i));
}
static int control(usb_disk_t *d,u8 type,u8 req,u16 value,u16 index,void *buf,u16 len) {
    assert(!d->hc->dead);address_unique();int i=lookup(d->address);nodes[i].queries++;
    assert(nodes[i].speed==(d->speed==2?0x400u:0u));
    if(d->hc->kind==1 && !d->speed) { u32 caps;assert(usb_tt_route(d,&caps));
        assert(caps==((1u<<30)|((u32)nodes[nodes[i].parent].address<<16)|((u32)nodes[i].port<<23))); }
    if(type==0 && req==5) { assert(value && value<128);nodes[i].address=value;sets++;address_unique();return 0; }
    if(type==0 && req==9) return 0;
    if(type==0x80 && req==6) {
        if(value==0x100) {
            u8 b[8]={18,1,0x10,1,0,0,0,8};b[4]=nodes[i].hub==1?9:nodes[i].hub==2?3:0;
            if(d->speed==2) { b[2]=0;b[3]=2;b[7]=64;b[6]=(u8)nodes[i].protocol; }
            if(dev_bad==1) b[3]=1;
            if(dev_bad==2) b[5]=1;
            if(dev_bad==3) b[6]=0;
            if(dev_bad==4) b[6]=3;
            if(dev_bad==5) b[7]=8;
            assert(len==8);memcpy(buf,b,8);return 8;
        }
        assert(value==0x200);u8 b[64]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,
                                    7,5,0x81,2,64,0,0,7,5,2,2,64,0,0};
        if(d->speed==2) { b[22]=b[29]=0;b[23]=b[30]=2; }
        if(nodes[i].hub==2) b[14]=3;
        if(nodes[i].hub==1) {
            b[2]=25;b[13]=1;b[14]=9;b[15]=0;b[16]=nodes[i].protocol==2?1:0;
            b[21]=3;b[22]=2;b[23]=0;b[24]=d->speed==2?12:255;
            if(nodes[i].protocol==2) {
                u8 alt[16]={9,4,0,1,1,9,0,2,0,7,5,0x81,3,2,0,12};
                memcpy(b+25,alt,16);b[2]=41;
                if(config_bad==6) b[28]=0;
            }
            if(config_bad==2) b[24]=0;
            if(config_bad==3) b[22]=0;
            if(config_bad==4) b[4]=2;
            if(config_bad==5) b[16]=2;
            if(config_bad==7) b[20]=1;
            if(config_bad==8) b[21]=2;
        }
        if(config_bad==1) b[18]=1;
        assert(len==9 || len==b[2]);memcpy(buf,b,len);return len;
    }
    assert(nodes[i].hub);
    if(type==0xA0 && req==6) {
        u8 b[16]={9,0x29,0,0,0,1,0};b[2]=ports_override?ports_override:nodes[i].ports;
        b[0]=7+2*((b[2]+8)/8);
        if(desc_bad==1) b[1]=0x30;
        if(desc_bad==3 && len>7) b[3]=1;
        if(desc_bad==4) b[0]--;
        assert(len==7 || len==b[0]);memcpy(buf,b,len);return desc_bad==2 && len>7?len-1:len;
    }
    if(type==0xA0 && req==0) { output(buf,len,nodes[i].state);return 4; }
    if(type==0x20 && req==1) { nodes[i].state&=~(1u<<(16+value));return 0; }
    assert(index && index<=nodes[i].ports);
    if(type==0xA3 && req==0) { output(buf,len,nodes[i].status[index]);return 4; }
    assert(type==0x23 && !len && (req==1 || req==3));
    if(value>=16 && req==1) { nodes[i].status[index]&=~(1u<<value);return 0; }
    if(value==8 && req==3) { nodes[i].status[index]|=0x100;return 0; }
    if(value==1 && req==1) {
        if(fail_disable) return -1;
        nodes[i].status[index]&=~2u;return 0;
    }
    assert(value==4 && req==3);
    for(int j=0;j<count;j++) if(nodes[j].parent==i && nodes[j].port==index && nodes[j].attached) {
        reset_node(j);nodes[i].status[index]=(nodes[i].status[index]&~0x600u)|nodes[j].speed;
    }
    nodes[i].status[index]|=0x100002;
    if(reset_change) nodes[i].status[index]|=0x10000;
    address_unique();return 0;
}
static u32 port_status(hc_t *h,int p) { (void)h;return root_on[p]?5:0; }
static int frames_wait(hc_t *h,unsigned n) { assert(n<=530);return !h->dead && !wait_fail; }
static int command(usb_disk_t *d,const u8 *c,u8 len,void *b,u32 n) {
    (void)len;
    if(!usb_route_live(d)) return -1;
    int i=lookup(d->address);assert(!nodes[i].hub);
    if(!c[0]) return 0;
    assert(c[0]==0x25 && n==8);u8 capacity[8]={0,0,15,255,0,0,2,0};memcpy(b,capacity,8);return 8;
}
static void fresh(void) {
    memset(nodes,0,sizeof(nodes));memset(hcs,0,sizeof(hcs));memset(root_on,0,sizeof(root_on));
    count=0;fail_disable=reset_change=wait_fail=ports_override=desc_bad=config_bad=dev_bad=sets=0;
    hcs[0].kind=2;hcs[0].ports=2;
}
static void scan(void) {
    enumeration_generation++;disk_count=0;hub_count=hub_refused=0;
    memset(hubs,0,sizeof(hubs));memset(disks,0,sizeof(disks));
    memset(root_on,0,sizeof(root_on)); /* production's global root-disable pass */
    next_address[0]=hcs[0].ports+1;
    for(int p=0;p<hcs[0].ports && !hcs[0].dead;p++) for(int i=0;i<count;i++) if(nodes[i].parent<0 && nodes[i].root==p && nodes[i].attached) {
        root_on[p]=1;reset_node(i);
        usb_disk_t d={.hc=&hcs[0],.port=p,.ep0=hcs[0].kind==1?64:8,.speed=hcs[0].kind==1?2:0};
        if(!enumerate_device(&d,p+1,0)) root_on[p]=0;
    }
    address_unique();
}
static void ordinary_tree(void) {
    fresh();int a=add(-1,0,1,3);add(a,1,0,0);int b=add(a,2,1,2);add(b,1,0,0);add(-1,1,0,0);
    scan();assert(disk_count==3 && hub_count==2 && !hub_refused);
}
int main(void) {
    ordinary_tree();assert(disks[0].address==3 && disks[1].address==5 && disks[2].address==2);
    for(int i=0;i<180;i++) { scan();assert(disk_count==3 && sets==5*(i+2)); }
    puts("rescan x181, root reservation, nested/global address uniqueness PASS");
    fresh();int a=add(-1,0,1,3);add(a,1,0,0);int b=add(a,2,1,1);add(b,1,0,0);
    int r=add(-1,1,1,1);add(r,1,0,0);scan();assert(disk_count==3);
    add(a,3,0,0);scan();assert(disk_count==4 && disks[2].address==6 && disks[3].address==7);
    puts("changed first-root tree cannot alias an old enabled second-root child PASS");
    ordinary_tree();usb_disk_t *d=&disks[1];d->write_allowed=1;nodes[0].status[2]|=0x10000;
    int before=nodes[2].queries;assert(!usb_route_live(d));assert(nodes[2].queries==before);
    assert(!d->write_allowed && !d->generation && d->route_dead);
    nodes[0].status[2]&=~0x10000u;d->route_dead=0; /* edge block must STILL win */
    assert(!usb_route_live(d));assert(usb_route_live(&disks[0]) && usb_route_live(&disks[2]));
    scan();assert(disk_count==3 && usb_route_live(&disks[1]));
    puts("ancestor-first checks, sticky edge block, sibling independence, rescan RO PASS");
    ordinary_tree();nodes[2].state=0x20000;assert(!usb_route_live(&disks[1]));assert(usb_route_live(&disks[0]));
    ordinary_tree();nodes[0].status[1]|=0x200;assert(!usb_route_live(&disks[0]));
    ordinary_tree();hubs[1].dev.parent=2;assert(!usb_route_live(&disks[1]));
    ordinary_tree();hubs[1].dev.hc=&hcs[1];assert(!usb_route_live(&disks[1]));
    ordinary_tree();hubs[1].dev.generation--;assert(!usb_route_live(&disks[1]));
    ordinary_tree();disks[1].downport=16;assert(!usb_route_live(&disks[1]));
    puts("hub power/change, speed, cycle, HC, epoch and port bounds PASS");
    fresh();add(-1,0,1,2);ports_override=16;scan();assert(!disk_count && hub_refused);
    fresh();add(-1,0,1,2);desc_bad=1;scan();assert(!disk_count && hub_refused);
    fresh();add(-1,0,1,2);fail_disable=1;scan();assert(!disk_count && hcs[0].dead);
    fresh();int p=add(-1,0,1,2);add(p,1,0,0);reset_change=1;scan();assert(!disk_count && !hcs[0].dead);
    fresh();p=add(-1,0,1,1);for(int n=0;n<5;n++) p=add(p,1,1,1);add(p,1,0,0);
    scan();assert(!disk_count && hub_count==USB_HUB_DEPTH && hub_refused);
    fresh();p=add(-1,0,1,12);for(int n=1;n<=12;n++) add(p,n,1,1);
    scan();assert(!disk_count && hub_count==USB_HUB_MAX && hub_refused);
    fresh();hcs[0].ports=15;p=add(-1,0,1,15);
    for(int n=1;n<=15;n++) {
        int branch=add(p,n,n<=7?1:2,n<=7?15:0);
        if(n<=7) for(int c=1;c<=15;c++) add(branch,c,2,0);
    }
    add(-1,1,0,0);scan();assert(disk_count==1 && hub_count==8 && hub_refused && next_address[0]==128);
    puts("descriptor/port/depth/pool/address limits, reset replacement, failed isolation fencing PASS");
    ordinary_tree();u8 good[32]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,7,5,0x81,2,64,0,0,7,5,2,2,64,0,0};
    int hub;usb_disk_t candidate={.hc=&hcs[0]};assert(usb_configure(&candidate,good,32,&hub) && !hub);
    for(unsigned len=0;len<32;len++) { memset(&candidate,0,sizeof(candidate));candidate.hc=&hcs[0];assert(!usb_configure(&candidate,good,len,&hub)); }
    for(unsigned at=0;at<32;at++) for(unsigned value=0;value<256;value++) {
        u8 b[32];memcpy(b,good,32);b[at]=value;memset(&candidate,0,sizeof(candidate));candidate.hc=&hcs[0];(void)usb_configure(&candidate,b,32,&hub);
    }
    puts("descriptor truncation/mutation ASan/UBSan PASS");
    for(unsigned proto=1;proto<=2;proto++) {
        fresh();hcs[0].kind=1;a=add(-1,0,1,3);nodes[a].protocol=proto;
        add(a,1,0,0);b=add(a,2,1,2);nodes[b].protocol=proto;add(b,1,0,0);add(-1,1,0,0);
        for(unsigned n=0;n<24;n++) {
            scan();assert(disk_count==3 && hub_count==2 && !hub_refused && !hcs[0].dead);
            assert(hubs[0].protocol==proto && hubs[1].protocol==proto);
            assert(disks[0].address==3 && disks[1].address==5 && disks[2].address==2);
        }
        disks[1].write_allowed=1;before=nodes[b].queries;nodes[a].status[2]&=~0x400u;
        assert(!usb_route_live(&disks[1]) && nodes[b].queries==before && !disks[1].write_allowed && !disks[1].generation);
        nodes[a].status[2]|=0x400;assert(!usb_route_live(&disks[1]));
        assert(usb_route_live(&disks[0]) && usb_route_live(&disks[2]));
        scan();assert(disk_count==3 && usb_route_live(&disks[1]));
    }
    puts("HS nested hubs: single/multi-TT descriptors (alt0 ONLY), 50 scans, private addresses and sticky speed/ancestor guards PASS");
    for(unsigned speed=0;speed<=0x600;speed+=0x200) if(speed!=0 && speed!=0x400) {
        fresh();hcs[0].kind=1;a=add(-1,0,1,2);b=add(a,1,0,0);nodes[b].speed=speed;add(a,2,0,0);
        scan();assert(disk_count==1 && !nodes[b].queries && !(nodes[a].status[1]&2));
    }
    for(dev_bad=1;dev_bad<=5;) {
        int bad=dev_bad;fresh();hcs[0].kind=1;add(-1,0,1,2);dev_bad=bad;scan();assert(!disk_count);dev_bad=bad+1;
    }
    for(int bad=1;bad<=8;bad++) {
        fresh();hcs[0].kind=1;a=add(-1,0,1,2);nodes[a].protocol=2;config_bad=bad;
        scan();assert(!hub_count && hub_refused);
    }
    for(int bad=1;bad<=4;bad++) {
        fresh();hcs[0].kind=1;add(-1,0,1,8);desc_bad=bad;scan();assert(!hub_count && hub_refused);
    }
    puts("HS refuses LS/invalid speed BEFORE child EP0; malformed protocol/version/interface/interrupt/bitmap/truncated/changed descriptors PASS");
    /* FS leaf uses nearest HS hub, not root-most hub. */
    for(unsigned proto=1;proto<=2;proto++) {
        fresh();hcs[0].kind=1;a=add(-1,0,1,3);nodes[a].protocol=proto;
        b=add(a,2,1,2);nodes[b].protocol=proto;int leaf=add(b,2,0,0);nodes[leaf].speed=0;
        add(a,1,0,0);add(-1,1,0,0);
        for(unsigned i=0;i<24;i++) {
            scan();assert(disk_count==3 && hub_count==2 && !hub_refused);
            d=&disks[1];u32 caps;assert(!d->speed && d->in_mps==64 && usb_tt_route(d,&caps));
            assert(caps==((1u<<30)|((u32)hubs[1].dev.address<<16)|(2u<<23)));
            assert(usb_route_live(d) && !d->write_allowed);
        }
        d->write_allowed=1;before=nodes[b].queries;nodes[a].status[2]&=~0x400u;
        assert(!usb_route_live(d) && !d->write_allowed && !d->generation && nodes[b].queries==before);
        assert(usb_route_live(&disks[0]) && usb_route_live(&disks[2]));
        scan();d=&disks[1];nodes[b].status[2]|=0x400;assert(!usb_route_live(d));
        scan();d=&disks[1];d->parent=0;u32 caps;assert(!usb_tt_route(d,&caps));
        scan();d=&disks[1];d->generation--;assert(!usb_tt_route(d,&caps));
        scan();d=&disks[1];hubs[1].dev.speed=0;assert(!usb_tt_route(d,&caps));
        scan();d=&disks[1];hubs[1].protocol=0;assert(!usb_tt_route(d,&caps));
        scan();d=&disks[1];hubs[1].dev.address=128;assert(!usb_tt_route(d,&caps));
    }
    fresh();hcs[0].kind=1;a=add(-1,0,1,2);b=add(a,1,1,1);nodes[b].speed=nodes[b].protocol=0;
    add(b,1,0,0);add(a,2,0,0);scan();assert(disk_count==1 && hub_count==1 && hub_refused);
    assert(nodes[b].queries==1 && !nodes[b].address && !(nodes[a].status[1]&2));
    puts("FS TT leaf: nearest HS hub/port, single+multi alt0, x48 scans, per-edge speed, stale epoch/route guards; FS hub refused before address PASS");
    return 0;
}
