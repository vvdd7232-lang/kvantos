/* Yutani-style shared pixel storage, adapted to bounded private ku_space.
   Independently implemented; upstream design reference and NCSA provenance:
   docs/PROCFS-SHM-MODULES.ru.md. */
#include "user_surface.h"
#ifdef KU_HOSTTEST
#include <string.h>
#else
#include "kernel.h"
#endif
_Static_assert(sizeof(ku_surface_info)==32,"surface descriptor wire");
int ku_surface_create(ku_surface *s,ku_space *space,uint32_t gen,uint32_t w,uint32_t h) {
    if(!s || s->live || !space || !space->live || space->pins || !gen || !w || !h ||
       w>4096 || h>4096 || w>KU_SHARED_PAGES*KU_PAGE/4u/h)return -1;
    uint32_t bytes=w*h*4u,pages=(bytes+KU_PAGE-1)/KU_PAGE;
    unsigned first=0,run=0;
    for(unsigned i=0;i<KU_PAGES;i++) {
        if(space->frames[i])run=0;else run++;
        if(run==2*pages) {first=i+1-run;break;}
    }
    if(run!=2*pages)return -1;
    memset(s,0,sizeof(*s));s->live=1;s->space=space;
    s->info=(ku_surface_info){2,sizeof(ku_surface_info),gen,w,h,w*4u,{first*KU_PAGE,(first+pages)*KU_PAGE}};
    for(unsigned i=0;i<2;i++) {
        if(ku_shared_create(&s->buffer[i],&space->ops,bytes) ||
           ku_map_shared(space,s->info.offset[i],&s->buffer[i],i?KU_R|KU_W:KU_R)) {
            ku_surface_destroy(s);return -1;
        }
    }
    return 0;
}
int ku_surface_destroy(ku_surface *s) {
    if(!s)return -1;
    if(!s->live)return 0;
    if(s->space->pins)return -1;
    for(unsigned i=0;i<2;i++) {
        if(s->buffer[i].live) {
            ku_unmap_shared(s->space,&s->buffer[i]);
            ku_shared_destroy(&s->buffer[i]);
        }
    }
    memset(s,0,sizeof(*s));return 0;
}
int ku_surface_queue(ku_surface *s,unsigned back,uint32_t sequence) {
    if(!s || !s->live || back>1 || back==s->front || s->pending ||
       s->sequence==0xffffffffu || sequence!=s->sequence+1)return -1;
    s->pending=(int)back+1;return 0;
}
int ku_surface_commit(ku_surface *s) {
    if(!s || !s->live || s->space->pins)return -1;
    if(!s->pending)return 0;
    unsigned next=(unsigned)s->pending-1;
    if(next>1 || s->front>1 || next==s->front || s->sequence==0xffffffffu)return -1;
    /* Check both objects/aliases before changing either set of permissions. */
    for(unsigned i=0;i<2;i++) {
        ku_shared *m=&s->buffer[i];unsigned count=0;
        if(!m->live || !m->owner)return -1;
        for(unsigned j=0;j<KU_PAGES;j++)if(s->space->shared[j]==m) {
            if(ku_check(s->space,j*KU_PAGE,KU_PAGE,KU_R))return -1;
            count++;
        }
        if(count<m->pages)return -1;
    }
    /* No user is running; all aliases in this space change together. */
    if(ku_protect_shared(s->space,&s->buffer[next],KU_R) ||
       ku_protect_shared(s->space,&s->buffer[s->front],KU_R|KU_W))return -1;
    s->front=next;s->sequence++;s->pending=0;s->presented=1;return 0;
}
int ku_surface_pixel(const ku_surface *s,uint32_t x,uint32_t y,uint32_t *argb) {
    if(!s || !s->live || !s->presented || !argb || x>=s->info.width || y>=s->info.height)return -1;
    const ku_shared *m=&s->buffer[s->front];
    uint32_t off=y*s->info.stride+x*4u;
    const uint8_t *p=m->ops.direct(m->ops.ctx,m->frames[off/KU_PAGE]);
    memcpy(argb,p+(off&(KU_PAGE-1)),4);return 0;
}
