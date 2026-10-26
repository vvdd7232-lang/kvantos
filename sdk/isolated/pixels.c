#include "kvuser.h"
static kvu_event event;
static kvu_surface surface;
static volatile uint32_t frames_submitted;
static const char status[]="CPL3 shared ARGB / F: front-write fault / Q: exit";
void _start(const kvu_boot *boot) __attribute__((noreturn));
void _start(const kvu_boot *boot) {
    if(!boot || boot->version!=2 || boot->bytes!=32)kvu_exit(10);
    kvu_request q=kvu_req(boot,KVU_QUERY,0,0,0,0);
    int caps=kvu_call(&q);
    if(caps<0 || !(caps&KVU_CAP_SURFACE))kvu_exit(11);
    q=kvu_req(boot,KVU_SURFACE_INFO,kvu_offset(&surface),sizeof(surface),0,0);
    if(kvu_call(&q)!=32 || surface.version!=2 || surface.generation!=boot->generation)kvu_exit(12);
    q=kvu_req(boot,KVU_STATUS,kvu_offset(status),sizeof(status)-1,0,0);
    if(kvu_call(&q))kvu_exit(13);
    uint32_t back=1,sequence=1,phase=0;
    for(;;) {
        q=kvu_req(boot,KVU_WAIT,kvu_offset(&event),sizeof(event),0,0);
        if(kvu_call(&q)<0)continue;
        if(event.type==5 || (event.type==2 && event.key=='q'))kvu_exit(0);
        if(event.type==2 && event.key=='f' && frames_submitted) {
            /* Deliberate hostile write: actual user page fault must stop ONLY
               this CPL3 app, then release both shared buffers. */
            volatile uint32_t *front=(volatile uint32_t *)(uintptr_t)(0xe00000u+surface.offset[back^1]);
            *front=0xffffffffu;kvu_exit(99);
        }
        if(event.type==4)phase+=event.delta_ms;
        if(event.type==1) {
            uint32_t *pixels=(uint32_t *)(uintptr_t)(0xe00000u+surface.offset[back]);
            for(uint32_t y=0;y<surface.height;y++)for(uint32_t x=0;x<surface.width;x++) {
                uint32_t alpha=64u+(x&127u);
                uint32_t red=(x+phase/8u)&255u,green=(y+phase/16u)&255u;
                pixels[y*(surface.stride/4u)+x]=(alpha<<24)|(red<<16)|(green<<8)|160u;
            }
            q=kvu_req(boot,KVU_SURFACE_PRESENT,back,sequence,0,0);q.token=event.id;
            if(kvu_call(&q))kvu_exit(14);
            sequence++;back^=1;frames_submitted++;
        }
        q=kvu_req(boot,KVU_COMPLETE,0,0,0,0);q.token=event.id;
        if(kvu_call(&q))kvu_exit(15);
    }
}
