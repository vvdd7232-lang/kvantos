/* Synchronous FS/HS hub trees; EHCI FS leaves use experimental async TT, no LS/FS hubs.
 * A rescan first disables ALL root links. Resetting each hub disables its
 * downstream links; explicitly disable them again before reusing addresses.
 * Never clear change latches during live I/O. A failed edge stays blocked
 * until the next enumeration epoch, including disconnect/reconnect races. */
#define USB_HUB_MAX 8
#define USB_HUB_DEPTH 4
#define USB_HUB_PORTS 15
static struct { usb_disk_t dev; u16 blocked; u8 ports,protocol; } hubs[USB_HUB_MAX];
static unsigned hub_count,hub_refused;
static u8 next_address[HC_MAX];
static int hub_feature(usb_disk_t *d,int set,unsigned port,unsigned feature) {
    return control(d,0x23,set?3:1,feature,port,0,0)==0;
}
static int hub_port(usb_disk_t *d,unsigned port,u32 *status) {
    u8 b[4];
    if(control(d,0xA3,0,0,port,b,4)!=4) return 0;
    *status=b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24);return 1;
}
static int hub_state(usb_disk_t *d,u32 *status) {
    u8 b[4];
    if(control(d,0xA0,0,0,0,b,4)!=4) return 0;
    *status=b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24);return 1;
}
/* Only immediate HS-parent TT routes are admitted. FS hubs/LS stay refused.
 * Protocol 2 hubs remain alt0/single-TT organization; QH uses physical port. */
static int usb_tt_route(usb_disk_t *d,u32 *caps) {
    if(d->hc->kind!=1 || d->speed || !d->parent || d->parent>hub_count || d->route_dead) return 0;
    unsigned i=d->parent-1;usb_disk_t *p=&hubs[i].dev;
    if(p->hc!=d->hc || p->speed!=2 || p->port!=d->port || p->route_dead ||
       p->generation!=enumeration_generation || !p->address || p->address>127 ||
       !d->downport || d->downport>hubs[i].ports || (hubs[i].blocked&(1u<<(d->downport-1))) ||
       (hubs[i].protocol!=1 && hubs[i].protocol!=2) ||
       (d->generation && d->generation!=enumeration_generation)) return 0;
    *caps=(1u<<30)|((u32)p->address<<16)|((u32)d->downport<<23);return 1;
}
static int hub_ready(unsigned speed,u32 s) {
    return (speed==0 || speed==2) && (s&0x103)==0x103 && !(s&0x1C) && !(s&0x1F0000) &&
        (s&0x600)==(speed==2?0x400u:0u);
}
static int usb_route_check(usb_disk_t *d,int poll) {
    if(d->route_dead || d->hc->dead) return 0;
    u32 s=port_status(d->hc,d->port),change=d->hc->kind?0x2A:0xA;
    if((s&5)!=5 || (s&change)) goto bad;
    /* Validate root-to-leaf BEFORE addressing any downstream hub. */
    u8 chain[USB_HUB_DEPTH],ports[USB_HUB_DEPTH],speeds[USB_HUB_DEPTH];
    unsigned n=0,parent=d->parent,port=d->downport,speed=d->speed;
    u32 tt;if(d->hc->kind==1 && d->speed==0 && !usb_tt_route(d,&tt)) goto bad;
    while(parent) {
        if(parent>hub_count || n==USB_HUB_DEPTH) goto bad;
        chain[n]=parent-1;ports[n]=port;speeds[n++]=(u8)speed;
        usb_disk_t *p=&hubs[parent-1].dev;
        if(p->hc!=d->hc || p->port!=d->port || p->generation!=enumeration_generation || p->route_dead) goto bad;
        parent=p->parent;port=p->downport;speed=p->speed;
    }
    while(n) {
        n--;unsigned i=chain[n],p=ports[n];
        if(!p || p>hubs[i].ports) goto bad;
        u16 bit=1u<<(p-1);
        if(hubs[i].blocked&bit) goto bad;
        if(!poll) continue;
        if(!hub_state(&hubs[i].dev,&s) || (s&0x30003)) {
            hubs[i].dev.route_dead=1;goto bad;
        }
        if(!hub_port(&hubs[i].dev,p,&s) || !hub_ready(speeds[n],s)) {
            hubs[i].blocked|=bit;goto bad;
        }
    }
    return 1;
bad:
    d->route_dead=1;d->write_allowed=0;d->generation=0;return 0;
}
static int usb_route_live(usb_disk_t *d) { return usb_route_check(d,1); }
/* Strict bounded descriptor walk. Keep the first BOT interface, but validate
 * the remainder too; refuse ambiguous hub/storage configurations. */
static int usb_configure(usb_disk_t *d,const u8 *b,unsigned len,int *is_hub) {
    if(len<9 || b[0]<9 || b[1]!=2 || (b[2]|((unsigned)b[3]<<8))!=len || !b[5]) return 0;
    int matching=0,found=0,hub=0,active_hub=0;unsigned hub_ep=0;
    for(unsigned pos=0;pos<len;) {
        if(len-pos<2) return 0;
        unsigned n=b[pos],type=b[pos+1];const u8 *p=b+pos;
        if(n<2 || n>len-pos) return 0;
        if(type==4) {
            if(n<9) return 0;
            if(matching && d->in && d->out) found=1;
            active_hub=0;
            matching=!found && p[3]==0 && p[5]==8 && p[6]==6 && p[7]==0x50;
            if(matching) { d->in=d->out=0;d->iface=p[2]; }
            if(p[3]==0 && p[5]==9 && p[6]==0) {
                /* Multi-TT hubs stay in alternate 0 (single-TT organization).
                 * Do not activate alt 1 or mistake it for another hub. */
                if(hub || p[2] || p[4]!=1 || p[7]>(d->speed==2?1:0)) return 0;
                hub=1+p[7];active_hub=1;d->iface=p[2];
            }
        } else if(type==5) {
            if(n<7) return 0;
            unsigned packet=p[4]|((unsigned)p[5]<<8);
            if(active_hub) {
                if(hub_ep++ || (p[2]&0xF0)!=0x80 || !(p[2]&15) || p[3]!=3 ||
                   !packet || packet>(d->speed==2?1024u:64u) || !p[6] ||
                   (d->speed==2 && p[6]>16)) return 0;
            } else if(matching && (p[3]&3)==2) {
                if(d->speed==2?packet!=512:(packet!=8 && packet!=16 && packet!=32 && packet!=64)) return 0;
                if(!(p[2]&15) || (p[2]&0x70)) return 0;
                if(p[2]&0x80) { if(d->in) return 0;d->in=p[2]&15;d->in_mps=packet; }
                else { if(d->out) return 0;d->out=p[2]&15;d->out_mps=packet; }
            }
        }
        pos+=n;
    }
    found|=matching && d->in && d->out;
    *is_hub=hub;
    if(hub && (found || hub_ep!=1 || b[4]!=1)) return 0;
    return hub || found;
}
static int enumerate_device(usb_disk_t *,unsigned,unsigned);
static int hub_disable(usb_disk_t *d,unsigned port) {
    u32 s;
    if(hub_feature(d,0,port,1) && hub_port(d,port,&s) && !(s&2)) return 1;
    usb_stop(d->hc);return 0; /* cannot prove address-zero isolation */
}
static int hub_reset_child(usb_disk_t *d,unsigned port,u8 *speed) {
    u32 s;
    if(!hub_port(d,port,&s) || !(s&1) || (s&0x1C)) return 0;
    /* Acknowledge only the pre-enumeration state, not a later replacement. */
    for(unsigned b=0;b<5;b++) if((s&(1u<<(16+b))) && !hub_feature(d,0,port,16+b)) return 0;
    if(!hub_feature(d,1,port,4)) return 0;
    for(unsigned n=0;n<20;n++) {
        if(!frames_wait(d->hc,10) || !hub_port(d,port,&s)) return 0;
        if(!(s&1) || (s&0xD0000)) return 0; /* new connection/suspend/overcurrent */
        if(!(s&16) && (s&0x100000)) {
            if(!hub_feature(d,0,port,20)) return 0;
            if((s&0x20000) && !hub_feature(d,0,port,17)) return 0;
            if(!hub_port(d,port,&s) || (s&0x200)) return 0;
            *speed=(s&0x400)?2:0;
            return (d->hc->kind==1 || !*speed) && hub_ready(*speed,s);
        }
    }
    return 0;
}
static int enumerate_hub(usb_disk_t *d,unsigned depth,unsigned protocol) {
    u8 desc[16];
    if(depth>=USB_HUB_DEPTH || hub_count==USB_HUB_MAX ||
       control(d,0xA0,6,0x2900,0,desc,7)!=7 || desc[1]!=0x29 || !desc[2] || desc[2]>USB_HUB_PORTS ||
       /* Some USB 1.1 hubs omit the unused hub bit in PortPwrCtrlMask
        * (QEMU eight-port hub: length 10, not 11). Neither bitmap is used. */
       desc[0]<7+(desc[2]+8)/8+(desc[2]+7)/8 || desc[0]>7+2*((desc[2]+8)/8) || !usb_route_live(d)) { hub_refused++;return 0; }
    if(d->hc->kind==1) {
        u8 head[7];memcpy(head,desc,7);unsigned len=desc[0];
        if(len!=7u+2u*((desc[2]+8u)/8u) || (desc[3]&3)==3 || (desc[3]&0x18)==0x18 || desc[4] ||
           control(d,0xA0,6,0x2900,0,desc,len)!=(int)len || memcmp(head,desc,7) || !usb_route_live(d)) {
            hub_refused++;return 0;
        }
    }
    u32 state;
    if(!hub_state(d,&state) || (state&3)) { hub_refused++;return 0; }
    for(unsigned b=0;b<2;b++) if((state&(1u<<(16+b))) && control(d,0x20,1,b,0,0,0)!=0) return 0;
    if(!hub_state(d,&state) || (state&0x30003)) return 0;
    unsigned start_disks=disk_count;
    unsigned i=hub_count++,ports=desc[2],delay=2u*desc[5]+20;
    d->generation=enumeration_generation;hubs[i].dev=*d;hubs[i].ports=ports;hubs[i].protocol=(u8)protocol;
    for(unsigned p=1;p<=ports;p++) if(!hub_feature(d,1,p,8)) goto failed;
    if(!frames_wait(d->hc,delay)) goto failed;
    /* All siblings must be disabled BEFORE any address can be reused. */
    for(unsigned p=1;p<=ports;p++) if(!hub_disable(d,p)) goto failed;
    for(unsigned p=1;p<=ports && disk_count<USB_MAX && !d->hc->dead;p++) {
        u32 s;
        if(!hub_port(d,p,&s)) goto failed;
        if(!(s&1)) continue;
        u8 speed;
        if(!hub_reset_child(d,p,&speed)) { if(!hub_disable(d,p)) goto failed;continue; }
        unsigned hc=d->hc-hcs,address=next_address[hc];
        if(address>127) { hub_refused++;if(!hub_disable(d,p)) goto failed;continue; }
        next_address[hc]++;
        usb_disk_t child;memset(&child,0,sizeof(child));
        child.hc=d->hc;child.port=d->port;child.parent=i+1;child.downport=p;child.speed=speed;child.ep0=speed==2?64:8;
        if(!enumerate_device(&child,address,depth+1) && !hub_disable(d,p)) goto failed;
    }
    if(usb_route_live(d)) return 1;
failed:
    hubs[i].dev.route_dead=1;disk_count=start_disks;hub_refused++;return 0;
}
static int enumerate_device(usb_disk_t *d,unsigned address,unsigned depth) {
    hc_t *h=d->hc;
    if(!address || address>127 || !usb_route_live(d) ||
       control(d,0x80,6,0x100,0,reply,8)!=8 || reply[0]<18 || reply[1]!=1) return 0;
    u8 mps=reply[7],device_class=reply[4],subclass=reply[5],protocol=reply[6];
    unsigned version=reply[2]|((unsigned)reply[3]<<8);
    if(mps!=8 && mps!=16 && mps!=32 && mps!=64) return 0;
    if(d->speed==2 && mps!=64) return 0;
    if(h->kind==1 && d->speed==0 && device_class==9) { hub_refused++;return 0; }
    d->ep0=mps;
    if(control(d,0,5,address,0,0,0)<0 || !frames_wait(h,3)) return 0;
    d->address=address;
    if(control(d,0x80,6,0x200,0,config,9)!=9 || config[1]!=2) return 0;
    unsigned len=config[2]|((unsigned)config[3]<<8);
    if(len<9 || len>sizeof(config) || control(d,0x80,6,0x200,0,config,len)!=(int)len) return 0;
    int hub=0;
    if(!usb_configure(d,config,len,&hub) ||
       (device_class==9 && !hub) || (hub && (device_class!=9 || subclass ||
       (d->speed==2?(protocol!=hub || version<0x200 || version>=0x300):protocol!=0)))) {
        if(device_class==9 || hub) hub_refused++;
        return 0;
    }
    if(control(d,0,9,config[5],0,0,0)<0 || !usb_route_live(d)) return 0;
    if(hub) return enumerate_hub(d,depth,protocol);
    u8 cdb[10]={0};
    for(int i=0;i<5;i++) {
        cdb[0]=0;
        if(command(d,cdb,6,0,0)==0) break;
        cdb[0]=3;cdb[4]=18;command(d,cdb,6,reply,18);cdb[4]=0;
        if(!frames_wait(h,50)) return 0;
    }
    memset(cdb,0,sizeof(cdb));cdb[0]=0x25;
    if(command(d,cdb,10,reply,8)!=8 || be32(reply+4)!=512 || !usb_route_live(d)) return 0;
    u32 last=be32(reply);
    if(last==0xFFFFFFFFu || disk_count==USB_MAX) return 0;
    d->generation=enumeration_generation;d->sectors=last+1;
    disks[disk_count++]=*d;return 1;
}
