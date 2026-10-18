/* Bounded offset-only event/draw/private-RAM services. No filesystem/media
   pointers and no direct kernel API table cross the user boundary. */
#include "user_service.h"
#ifdef KU_HOSTTEST
#include <string.h>
#else
#include "kernel.h"
#endif
_Static_assert(sizeof(ks_request)==40,"request wire layout");
_Static_assert(sizeof(ks_event)==48,"event wire layout");
_Static_assert(sizeof(ks_rect)==24 && sizeof(ks_text_wire)==24 && sizeof(ks_line)==24,"draw wire layout");
int ks_init(ks_service *s,const ku_space *space,uint32_t gen,uint32_t w,uint32_t h,ks_sink sink,void *ctx) {
    if(!s || !space || !space->live || !gen || !w || !h || w>4096 || h>4096 || s->live)return KS_INVALID;
    memset(s,0,sizeof(*s));s->space=space;s->generation=gen;s->width=w;s->height=h;
    s->sink=sink;s->sink_ctx=ctx;s->live=1;return 0;
}
void ks_destroy(ks_service *s) {if(s)memset(s,0,sizeof(*s));}
int ks_post(ks_service *s,uint32_t type,uint32_t delta,int32_t x,int32_t y,int32_t key,int32_t button) {
    if(!s || !s->live || type<1 || type>5)return KS_INVALID;
    if(s->pending)return KS_BUSY;
    if(s->next_event==0xffffffffu)return KS_QUOTA;
    s->event=(ks_event){KS_VERSION,sizeof(ks_event),s->generation,++s->next_event,type,delta>250?250:delta,s->width,s->height,x,y,key,button};
    s->pending=1;s->delivered=0;s->draw_left=64;s->text_left=KS_TEXT_BUDGET;return 0;
}
static int filename(ks_service *s,char *name,uint32_t off,uint32_t n) {
    if(n<2 || n>32)return KS_INVALID;
    if(ku_copyin(s->space,name,off,n))return KS_FAULT;
    if(name[n-1] || !strcmp(name,".") || !strcmp(name,".."))return KS_INVALID;
    for(unsigned i=0;i<n-1;i++) {
        unsigned c=(uint8_t)name[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='-' || c=='_'))return KS_INVALID;
    }
    return 0;
}
static int node(ks_service *s,const char *name) {
    for(unsigned i=0;i<KS_FILES;i++)if(s->files[i].used && !strcmp(name,s->files[i].name))return (int)i;
    return -1;
}
static int handle(ks_service *s,uint32_t cookie,unsigned rights) {
    for(unsigned i=0;i<KS_HANDLES;i++)if(cookie && s->handles[i].cookie==cookie) {
        if((s->handles[i].rights&rights)!=rights)return KS_DENIED;
        return (int)i;
    }
    return KS_STALE;
}
static int32_t open_file(ks_service *s,const ks_request *r) {
    char name[32];int rc=filename(s,name,r->a,r->b);if(rc)return rc;
    unsigned flags=r->c;
    if(r->d || !(flags&3) || (flags&~15u) || ((flags&12) && !(flags&KS_OPEN_W)))return KS_INVALID;
    int f=node(s,name),h=-1,fresh=f<0;
    if(fresh && !(flags&KS_OPEN_CREATE))return KS_MISSING;
    if(fresh)for(unsigned i=0;i<KS_FILES;i++)if(!s->files[i].used) {f=(int)i;break;}
    for(unsigned i=0;i<KS_HANDLES;i++)if(!s->handles[i].cookie) {h=(int)i;break;}
    if(f<0 || h<0 || s->next_handle==0x7fffffffu)return KS_QUOTA;
    /* All validation and capacity checks precede creation/truncation. */
    if(fresh) {memset(&s->files[f],0,sizeof(s->files[f]));strcpy(s->files[f].name,name);s->files[f].used=1;}
    if(flags&KS_OPEN_TRUNC) {memset(s->files[f].data,0,KS_FILE_BYTES);s->files[f].size=0;}
    s->files[f].refs++;s->handles[h].file=(uint32_t)f;s->handles[h].rights=flags&3;
    return (int32_t)(s->handles[h].cookie=++s->next_handle);
}
static int32_t file_io(ks_service *s,const ks_request *r) {
    int h=handle(s,r->a,r->op==KS_READ?KS_OPEN_R:KS_OPEN_W);if(h<0)return h;
    unsigned f=s->handles[h].file;uint32_t n=r->c;
    if(r->d>KS_FILE_BYTES || n>KS_FILE_BYTES-r->d)return KS_QUOTA;
    if(r->op==KS_READ) {
        if(r->d>=s->files[f].size)n=0;
        else if(n>s->files[f].size-r->d)n=s->files[f].size-r->d;
        if(ku_copyout(s->space,r->b,s->files[f].data+r->d,n))return KS_FAULT;
    } else {
        if(ku_copyin(s->space,s->scratch,r->b,n))return KS_FAULT;
        if(n) {
            memcpy(s->files[f].data+r->d,s->scratch,n);
            if(r->d+n>s->files[f].size)s->files[f].size=r->d+n;
        }
    }
    return (int32_t)n;
}
static int32_t draw(ks_service *s,const ks_request *r) {
    if(!s->pending || !s->delivered || s->event.type!=KS_EVENT_DRAW || r->token!=s->event.id)return KS_STALE;
    if(!r->b || r->b>KS_COMMANDS || r->c || r->d)return KS_INVALID;
    if(r->b>s->draw_left)return KS_QUOTA;
    if(ku_copyin(s->space,s->scratch,r->a,r->b*sizeof(ks_rect)))return KS_FAULT;
    unsigned count=0;
    for(unsigned i=0;i<r->b;i++) {
        ks_rect c;memcpy(&c,s->scratch+i*sizeof(c),sizeof(c));
        if((c.type!=1 && c.type!=2) || c.w<=0 || c.h<=0)return KS_INVALID;
        int64_t x1=c.x,y1=c.y,x2=x1+c.w,y2=y1+c.h;
        if(x1<0)x1=0;
        if(y1<0)y1=0;
        if(x2>s->width)x2=s->width;
        if(y2>s->height)y2=s->height;
        if(x2<=x1 || y2<=y1)continue;
        c.x=(int32_t)x1;c.y=(int32_t)y1;c.w=(int32_t)(x2-x1);c.h=(int32_t)(y2-y1);c.color&=0xffffff;
        s->rects[count++]=c;
    }
    s->draw_left-=r->b;
    if(s->sink)s->sink(s->sink_ctx,s->rects,count);
    return (int32_t)count;
}
/* Validate the complete bounded UTF-8 snapshot before exposing it to a sink.
   No embedded NUL/control, overlong, surrogate, truncated or >U+10FFFF scalar. */
static int glyph_count(const uint8_t *p,uint32_t n) {
    unsigned i=0,glyphs=0;
    while(i<n) {
        uint32_t c=p[i++],cp,min;unsigned more;
        if(c<128) {if(c<32 || c==127)return -1;glyphs++;continue;}
        if(c>=0xc2 && c<=0xdf) {cp=c&31;more=1;min=128;}
        else if(c>=0xe0 && c<=0xef) {cp=c&15;more=2;min=2048;}
        else if(c>=0xf0 && c<=0xf4) {cp=c&7;more=3;min=65536;}
        else return -1;
        if(more>n-i)return -1;
        while(more--) {c=p[i++];if((c&0xc0)!=0x80)return -1;cp=(cp<<6)|(c&63);}
        if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return -1;
        glyphs++;
    }
    return (int)glyphs;
}
static int32_t text(ks_service *s,const ks_request *r) {
    if(!s->pending || !s->delivered || s->event.type!=KS_EVENT_DRAW || r->token!=s->event.id)return KS_STALE;
    if(r->b!=sizeof(ks_text_wire) || r->c || r->d)return KS_INVALID;
    if(!s->text_sink)return KS_DENIED;
    ks_text_wire w;if(ku_copyin(s->space,&w,r->a,sizeof(w)))return KS_FAULT;
    if(!w.length || w.length>KS_TEXT_BYTES)return KS_INVALID;
    if(!s->draw_left || w.length>s->text_left)return KS_QUOTA;
    ks_text t;memset(&t,0,sizeof(t));
    if(ku_copyin(s->space,t.bytes,w.offset,w.length))return KS_FAULT;
    int glyphs=glyph_count((const uint8_t *)t.bytes,w.length);if(glyphs<0)return KS_INVALID;
    t.x=w.x;t.y=w.y;t.color=w.color&0xffffff;t.bg=w.bg==0xffffffff?w.bg:w.bg&0xffffff;
    s->draw_left--;s->text_left-=w.length;
    if((int64_t)w.x+glyphs*8<=0 || w.x>=(int32_t)s->width ||
       (int64_t)w.y+16<=0 || w.y>=(int32_t)s->height)return 0;
    s->text_sink(s->sink_ctx,&t);return (int32_t)w.length;
}
/* Signed-16-bit endpoints bound every clipping product below UINT32_MAX.
   This avoids slow/unavailable compiler 64-bit division helpers on i386. */
static unsigned outcode(int32_t x,int32_t y,uint32_t w,uint32_t h) {
    return (x<0?1:x>=(int32_t)w?2:0)|(y<0?4:y>=(int32_t)h?8:0);
}
static int32_t ratio(int32_t a,int32_t b,int32_t c) {
    uint32_t x=(uint32_t)(a<0?-a:a),y=(uint32_t)(b<0?-b:b),d=(uint32_t)(c<0?-c:c);
    int32_t q=(int32_t)(x*y/d);return (a<0)^(b<0)^(c<0)?-q:q;
}
static int32_t line(ks_service *s,const ks_request *r) {
    if(!s->pending || !s->delivered || s->event.type!=KS_EVENT_DRAW || r->token!=s->event.id)return KS_STALE;
    if(r->b!=sizeof(ks_line) || r->c || r->d)return KS_INVALID;
    if(!s->line_sink)return KS_DENIED;
    ks_line l;if(ku_copyin(s->space,&l,r->a,sizeof(l)))return KS_FAULT;
    if(l.reserved || l.x0<-32768 || l.x0>32767 || l.y0<-32768 || l.y0>32767 ||
       l.x1<-32768 || l.x1>32767 || l.y1<-32768 || l.y1>32767)return KS_INVALID;
    if(!s->draw_left)return KS_QUOTA;
    for(unsigned step=0;step<8;step++) {
        unsigned a=outcode(l.x0,l.y0,s->width,s->height),b=outcode(l.x1,l.y1,s->width,s->height);
        if(!(a|b)) {l.color&=0xffffff;s->draw_left--;s->line_sink(s->sink_ctx,&l);return 1;}
        if(a&b) {s->draw_left--;return 0;}
        unsigned pick=a?a:b;int32_t x,y,dx=l.x1-l.x0,dy=l.y1-l.y0;
        if(pick&12) {
            if(!dy)return KS_INVALID;
            y=pick&4?0:(int32_t)s->height-1;x=l.x0+ratio(dx,y-l.y0,dy);
        } else {
            if(!dx)return KS_INVALID;
            x=pick&1?0:(int32_t)s->width-1;y=l.y0+ratio(dy,x-l.x0,dx);
        }
        if(a) {l.x0=x;l.y0=y;}else {l.x1=x;l.y1=y;}
    }
    return KS_INVALID; /* fail closed if integer rounding fails to converge */
}
static int32_t list(ks_service *s,const ks_request *r) {
    if(r->d)return KS_INVALID;
    if(r->a>=KS_FILES)return KS_MISSING;
    unsigned at=0;
    for(unsigned i=0;i<KS_FILES;i++)if(s->files[i].used) {
        if(at++!=r->a)continue;
        if(ku_check(s->space,r->b,40,KU_R|KU_W) || ku_check(s->space,r->c,4,KU_R|KU_W))return KS_FAULT;
        if(r->b<r->c+4 && r->c<r->b+40)return KS_INVALID;
        /* Both destinations preflighted before either write. Names snapshot,
           no global RAMFS/KvFS entries or handles are exposed. */
        char name[40];memset(name,0,sizeof(name));memcpy(name,s->files[i].name,32);
        uint32_t size=s->files[i].size;
        if(ku_copyout(s->space,r->b,name,40) || ku_copyout(s->space,r->c,&size,4))return KS_FAULT;
        return 0;
    }
    return KS_MISSING;
}
int32_t ks_dispatch(ks_service *s,uint32_t off,uint32_t size) {
    if(!s || !s->live || !s->space->live || !s->space->pins)return KS_INVALID;
    if(size!=sizeof(ks_request))return KS_INVALID;
    ks_request r;if(ku_copyin(s->space,&r,off,sizeof(r)))return KS_FAULT;
    if(r.version!=KS_VERSION || r.bytes!=sizeof(r) || r.flags)return KS_INVALID;
    if(r.generation!=s->generation)return KS_DENIED;
    if(r.token && r.op!=KS_COMPLETE && r.op!=KS_DRAW && r.op!=KS_TEXT && r.op!=KS_LINE)return KS_INVALID;
    switch(r.op) {
    case KS_WAIT:
        if(r.c || r.d || r.b!=sizeof(ks_event))return KS_INVALID;
        if(!s->pending)return KS_AGAIN;
        if(ku_copyout(s->space,r.a,&s->event,sizeof(s->event)))return KS_FAULT;
        s->delivered=1;return sizeof(ks_event);
    case KS_COMPLETE:
        if(r.a || r.b || r.c || r.d)return KS_INVALID;
        if(!s->pending || !s->delivered || r.token!=s->event.id)return KS_STALE;
        s->pending=s->delivered=0;return 0;
    case KS_DRAW:return draw(s,&r);
    case KS_TEXT:return text(s,&r);
    case KS_LINE:return line(s,&r);
    case KS_LIST:return list(s,&r);
    case KS_QUERY:
        if(r.a || r.b || r.c || r.d)return KS_INVALID;
        return KS_CAP_RECT|KS_CAP_STATUS|KS_CAP_INFO|KS_CAP_LIST|KS_CAP_CLOCK|(s->text_sink?KS_CAP_TEXT:0)|(s->line_sink?KS_CAP_LINE:0);
    case KS_STATUS: {
        if(r.b>95 || r.c || r.d)return KS_INVALID;
        char copy[96];memset(copy,0,sizeof(copy));
        if(ku_copyin(s->space,copy,r.a,r.b))return KS_FAULT;
        if(glyph_count((const uint8_t *)copy,r.b)<0)return KS_INVALID;
        memcpy(s->status,copy,sizeof(copy));return 0;
    }
    case KS_INFO: {
        if(r.b!=16 || r.c || r.d)return KS_INVALID;
        uint32_t info[4]={1,16,s->hz,s->language};
        return ku_copyout(s->space,r.a,info,sizeof(info))?KS_FAULT:16;
    }
    case KS_CLOCK: {
        if(r.b!=20 || r.c || r.d)return KS_INVALID;
        if(!s->clock_valid || s->clock[0]>23 || s->clock[1]>59 || s->clock[2]>59)return KS_AGAIN;
        uint32_t value[5]={1,20,s->clock[0],s->clock[1],s->clock[2]};
        return ku_copyout(s->space,r.a,value,sizeof(value))?KS_FAULT:20;
    }
    case KS_OPEN:return open_file(s,&r);
    case KS_READ:case KS_WRITE:return file_io(s,&r);
    case KS_CLOSE: {
        if(r.b || r.c || r.d)return KS_INVALID;
        int h=handle(s,r.a,0);if(h<0)return h;
        s->files[s->handles[h].file].refs--;memset(&s->handles[h],0,sizeof(s->handles[h]));return 0;
    }
    case KS_DELETE: {
        if(r.c || r.d)return KS_INVALID;
        char name[32];int rc=filename(s,name,r.a,r.b);if(rc)return rc;
        int f=node(s,name);if(f<0)return KS_MISSING;
        if(s->files[f].refs)return KS_BUSY;
        memset(&s->files[f],0,sizeof(s->files[f]));return 0;
    }
    default:return KS_UNKNOWN;
    }
}
