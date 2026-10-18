#define main page_model_main_unused
#include "test_user_space.c"
#undef main
#include "user_service.h"
static ks_service sa,sb;
static ks_rect rendered[32];static unsigned render_count,render_calls;
static void sink(void *ctx,const ks_rect *rects,unsigned n) {
    (void)ctx;assert(n<=32);memcpy(rendered,rects,n*sizeof(*rects));render_count=n;render_calls++;
    for(unsigned i=0;i<n;i++)assert(rects[i].x>=0 && rects[i].y>=0 && rects[i].w>0 && rects[i].h>0 && rects[i].x+rects[i].w<=100 && rects[i].y+rects[i].h<=80);
}
static ks_request request(unsigned op,unsigned a0,unsigned b0,unsigned c0,unsigned d0) {
    return (ks_request){KS_VERSION,sizeof(ks_request),op,0,11,0,a0,b0,c0,d0};
}
static int submit(ks_service *s,ku_space *m,ks_request r) {
    assert(!ku_copyout(m,0,&r,sizeof(r)));return ks_dispatch(s,0,sizeof(r));
}
static void text_in(ku_space *m,unsigned off,const char *s) {assert(!ku_copyout(m,off,s,(uint32_t)strlen(s)+1));}
static void service_suite(int arch) {
    uint32_t root=setup(arch);memset(&sa,0,sizeof(sa));memset(&sb,0,sizeof(sb));
    assert(!ku_create(&a,&ops,root,arch,arch) && !ku_create(&b,&ops,root,arch,arch));
    assert(!ku_map(&a,0,KU_R|KU_W) && !ku_map(&a,4096,KU_R) && !ku_map(&a,8192,KU_R|KU_W));
    assert(!ku_map(&b,0,KU_R|KU_W) && !ku_map(&b,8192,KU_R|KU_W));
    assert(!ku_pin(&a) && !ku_pin(&b));
    assert(!ks_init(&sa,&a,11,100,80,sink,NULL) && !ks_init(&sb,&b,22,100,80,sink,NULL));
    ks_request r=request(KS_WAIT,8192,sizeof(ks_event),0,0);
    assert(submit(&sa,&a,r)==KS_AGAIN);
    assert(!ks_post(&sa,KS_EVENT_DRAW,999,0,0,0,0));
    assert(ks_post(&sa,KS_EVENT_DRAW,0,0,0,0,0)==KS_BUSY);
    ks_request done=request(KS_COMPLETE,0,0,0,0);done.token=sa.event.id;
    assert(submit(&sa,&a,done)==KS_STALE);
    r.a=4096;assert(submit(&sa,&a,r)==KS_FAULT && !sa.delivered);
    r.a=KU_SIZE-20;assert(submit(&sa,&a,r)==KS_FAULT && !sa.delivered);
    r.a=8192;assert(submit(&sa,&a,r)==sizeof(ks_event));
    ks_event ev;assert(!ku_copyin(&a,&ev,8192,sizeof(ev)) && ev.generation==11 && ev.delta_ms==250 && ev.width==100 && ev.height==80);
    ks_rect commands[2]={{1,0xffffffff,-10,-20,50,60},{2,1,2147483647,2147483647,2147483647,2147483647}};
    assert(!ku_copyout(&a,1024,commands,sizeof(commands)));
    ks_request draw=request(KS_DRAW,1024,2,0,0);draw.token=ev.id;
    assert(submit(&sa,&a,draw)==1 && render_count==1 && rendered[0].x==0 && rendered[0].y==0 && rendered[0].w==40 && rendered[0].h==40 && rendered[0].color==0xffffff);
    unsigned before=render_calls,budget=sa.draw_left;
    commands[1].w=-1;assert(!ku_copyout(&a,1024,commands,sizeof(commands)));
    assert(submit(&sa,&a,draw)==KS_INVALID && render_calls==before && sa.draw_left==budget);
    draw.token++;assert(submit(&sa,&a,draw)==KS_STALE);draw.token--;
    commands[1].w=1;assert(!ku_copyout(&a,1024,commands,sizeof(commands)));
    sa.draw_left=1;assert(submit(&sa,&a,draw)==KS_QUOTA && render_calls==before);
    assert(!submit(&sa,&a,done) && submit(&sa,&a,done)==KS_STALE && submit(&sa,&a,draw)==KS_STALE);
    assert(!ks_post(&sa,KS_EVENT_TICK,17,0,0,0,0));
    assert(submit(&sa,&a,r)==sizeof(ks_event));draw.token=sa.event.id;
    assert(submit(&sa,&a,draw)==KS_STALE);
    text_in(&a,256,"test.txt");text_in(&a,512,"private A");text_in(&b,256,"test.txt");text_in(&b,512,"private B");
    ks_request open=request(KS_OPEN,256,9,KS_OPEN_R|KS_OPEN_W|KS_OPEN_CREATE,0);
    int h=submit(&sa,&a,open);assert(h>0);
    r=request(KS_WRITE,(unsigned)h,512,10,0);assert(submit(&sa,&a,r)==10);
    ks_request foreign=request(KS_READ,(unsigned)h,8192,10,0);foreign.generation=22;
    assert(submit(&sb,&b,foreign)==KS_STALE);
    ks_request open_b=open;open_b.generation=22;int hb=submit(&sb,&b,open_b);assert(hb>0);
    r=request(KS_WRITE,(unsigned)hb,512,10,0);r.generation=22;assert(submit(&sb,&b,r)==10);
    r=request(KS_READ,(unsigned)h,8192,10,0);assert(submit(&sa,&a,r)==10);
    char name[32];assert(!ku_copyin(&a,name,8192,10) && !strcmp(name,"private A"));
    r.a=(unsigned)hb;r.generation=22;assert(submit(&sb,&b,r)==10);
    assert(!ku_copyin(&b,name,8192,10) && !strcmp(name,"private B"));
    r=request(KS_WRITE,(unsigned)h,KU_SIZE-2,10,0);assert(submit(&sa,&a,r)==KS_FAULT && !memcmp(sa.files[0].data,"private A",10));
    r=request(KS_WRITE,(unsigned)h,512,1024,1);assert(submit(&sa,&a,r)==KS_QUOTA && sa.files[0].size==10);
    r=request(KS_READ,(unsigned)h,4096,10,0);assert(submit(&sa,&a,r)==KS_FAULT);
    r=request(KS_DELETE,256,9,0,0);assert(submit(&sa,&a,r)==KS_BUSY);
    r=request(KS_CLOSE,(unsigned)h,0,0,0);assert(!submit(&sa,&a,r) && submit(&sa,&a,r)==KS_STALE);
    open.c=KS_OPEN_R;int ro=submit(&sa,&a,open);assert(ro>h);
    r=request(KS_WRITE,(unsigned)ro,512,10,0);assert(submit(&sa,&a,r)==KS_DENIED);
    r=request(KS_CLOSE,(unsigned)ro,0,0,0);assert(!submit(&sa,&a,r));
    text_in(&a,256,"../a");open.b=5;assert(submit(&sa,&a,open)==KS_INVALID);
    text_in(&a,256,"test.txt");open.b=9;open.c=KS_OPEN_R|KS_OPEN_W;
    unsigned handles[8];for(unsigned i=0;i<8;i++){int v=submit(&sa,&a,open);assert(v>0);handles[i]=(unsigned)v;}
    open.c|=KS_OPEN_TRUNC;assert(submit(&sa,&a,open)==KS_QUOTA && sa.files[0].size==10);
    for(unsigned i=0;i<8;i++)assert(!submit(&sa,&a,request(KS_CLOSE,handles[i],0,0,0)));
    sa.next_handle=0x7fffffff;assert(submit(&sa,&a,open)==KS_QUOTA && sa.files[0].size==10);
    sa.next_handle=100;assert(!submit(&sa,&a,request(KS_DELETE,256,9,0,0)));
    assert(submit(&sa,&a,request(KS_READ,handles[0],8192,10,0))==KS_STALE);
    r=request(999,0,0,0,0);assert(submit(&sa,&a,r)==KS_UNKNOWN);
    r.generation=22;assert(submit(&sa,&a,r)==KS_DENIED);
    r.generation=11;r.flags=1;assert(submit(&sa,&a,r)==KS_INVALID);
    r.flags=0;r.bytes--;assert(submit(&sa,&a,r)==KS_INVALID);
    assert(ks_dispatch(&sa,0,39)==KS_INVALID && ks_dispatch(&sa,KU_SIZE-20,40)==KS_FAULT);
    uint32_t seed=4242;
    for(unsigned i=0;i<20000;i++) {
        seed=seed*1664525u+1013904223u;r=request(seed%(KS_CLOCK+1),seed,seed^0xa5a5,seed>>8,seed<<8);
        r.token=(i&1)?0:seed;r.generation=(i%3)?11:seed;
        (void)submit(&sa,&a,r);
    }
    ks_destroy(&sa);assert(ks_dispatch(&sa,0,40)==KS_INVALID);
    assert(!ks_init(&sa,&a,99,100,80,sink,NULL));
    r=request(KS_READ,1,8192,10,0);assert(submit(&sa,&a,r)==KS_DENIED);
    r.generation=99;assert(submit(&sa,&a,r)==KS_STALE);
    ks_destroy(&sa);ks_destroy(&sb);assert(!ku_unpin(&a) && !ku_unpin(&b));
    assert(!ku_destroy(&a) && !ku_destroy(&b) && !live);
    printf("PASS %s event tokens/generation, atomic clipped draw quotas, private RAM files/rights/stale handles, copies, 20000 mutated requests\n",arch?"x64":"i386");
}
static ks_text rendered_text;static unsigned text_calls;
static void text_sink(void *ctx,const ks_text *t) {
    (void)ctx;assert(t->x>-1024 && t->x<100 && t->y>-16 && t->y<80);
    assert(t->bytes[127]==0);rendered_text=*t;text_calls++;
}
static int text_send(ks_text_wire w) {
    assert(!ku_copyout(&a,256,&w,sizeof(w)));
    ks_request r=request(KS_TEXT,256,sizeof(w),0,0);r.token=sa.event.id;
    return submit(&sa,&a,r);
}
static void text_suite(int arch) {
    uint32_t root=setup(arch);memset(&sa,0,sizeof(sa));text_calls=0;
    assert(!ku_create(&a,&ops,root,arch,arch));
    assert(!ku_map(&a,0,KU_R|KU_W) && !ku_map(&a,8192,KU_R|KU_W) && !ku_pin(&a));
    assert(!ks_init(&sa,&a,11,100,80,sink,NULL));
    assert(submit(&sa,&a,request(KS_QUERY,0,0,0,0))==(KS_CAP_RECT|KS_CAP_STATUS|KS_CAP_INFO|KS_CAP_LIST|KS_CAP_CLOCK));
    assert(!ks_post(&sa,KS_EVENT_DRAW,0,0,0,0,0));
    assert(submit(&sa,&a,request(KS_WAIT,8192,sizeof(ks_event),0,0))==sizeof(ks_event));
    const char valid[]="A \xd0\x96";assert(!ku_copyout(&a,512,valid,sizeof(valid)));
    ks_text_wire w={8,10,0xff123456,0xffffffff,512,4};
    assert(text_send(w)==KS_DENIED && sa.draw_left==64 && sa.text_left==1024);
    sa.text_sink=text_sink;
    assert(submit(&sa,&a,request(KS_QUERY,0,0,0,0))==(KS_CAP_RECT|KS_CAP_TEXT|KS_CAP_STATUS|KS_CAP_INFO|KS_CAP_LIST|KS_CAP_CLOCK));
    assert(submit(&sa,&a,request(KS_QUERY,1,0,0,0))==KS_INVALID);
    assert(text_send(w)==4 && text_calls==1 && !memcmp(rendered_text.bytes,valid,5));
    assert(rendered_text.color==0x123456 && rendered_text.bg==0xffffffff);
    text_in(&a,512,"other");assert(!memcmp(rendered_text.bytes,valid,5)); /* snapshot lifetime */
    static const uint8_t bad[][5]={{0xc0,0x80},{0xed,0xa0,0x80},{0xe2,0x82},{0xff},{0xf4,0x90,0x80,0x80},{'a',0,'b'},{127},{31}};
    static const uint8_t lens[]={2,3,2,1,4,3,1,1};
    unsigned calls_before=text_calls,draw_before=sa.draw_left,bytes_before=sa.text_left;
    for(unsigned i=0;i<sizeof(lens);i++) {
        assert(!ku_copyout(&a,512,bad[i],lens[i]));w.length=lens[i];
        assert(text_send(w)==KS_INVALID && text_calls==calls_before && sa.draw_left==draw_before && sa.text_left==bytes_before);
    }
    w.length=1;w.offset=KU_SIZE-1;assert(text_send(w)==KS_FAULT);w.offset=512;
    w.length=128;assert(text_send(w)==KS_INVALID);w.length=0;assert(text_send(w)==KS_INVALID);
    uint8_t max[127];memset(max,'x',sizeof(max));assert(!ku_copyout(&a,512,max,sizeof(max)));
    w.length=127;assert(text_send(w)==127 && rendered_text.bytes[126]=='x' && rendered_text.bytes[127]==0);
    calls_before=text_calls;draw_before=sa.draw_left;sa.text_left=126;
    assert(text_send(w)==KS_QUOTA && text_calls==calls_before && sa.draw_left==draw_before && sa.text_left==126);
    sa.text_left=1024;sa.draw_left=0;assert(text_send(w)==KS_QUOTA && text_calls==calls_before);
    sa.draw_left=64;w.x=-2147483647-1;assert(text_send(w)==0 && text_calls==calls_before);
    w.x=2147483647;assert(text_send(w)==0 && text_calls==calls_before);
    w.x=-8;w.y=-1;w.length=3;w.bg=0xaa765432;
    assert(text_send(w)==3 && rendered_text.bg==0x765432);
    text_in(&a,512,"private status");
    assert(!submit(&sa,&a,request(KS_STATUS,512,14,0,0)) && !strcmp(sa.status,"private status"));
    assert(submit(&sa,&a,request(KS_STATUS,512,96,0,0))==KS_INVALID && !strcmp(sa.status,"private status"));
    assert(submit(&sa,&a,request(KS_STATUS,KU_SIZE-1,14,0,0))==KS_FAULT && !strcmp(sa.status,"private status"));
    assert(!ku_copyout(&a,512,bad[1],3));
    assert(submit(&sa,&a,request(KS_STATUS,512,3,0,0))==KS_INVALID && !strcmp(sa.status,"private status"));
    assert(!submit(&sa,&a,request(KS_STATUS,0,0,0,0)) && !sa.status[0]);
    sa.hz=500;sa.language=1;
    assert(submit(&sa,&a,request(KS_INFO,8192,16,0,0))==16);
    uint32_t info[4];assert(!ku_copyin(&a,info,8192,sizeof(info)) && info[0]==1 && info[1]==16 && info[2]==500 && info[3]==1);
    assert(submit(&sa,&a,request(KS_INFO,KU_SIZE-4,16,0,0))==KS_FAULT);
    assert(submit(&sa,&a,request(KS_INFO,8192,15,0,0))==KS_INVALID);
    uint32_t seed=773;
    for(unsigned run=0;run<5000;run++) {
        for(unsigned i=0;i<127;i++) {seed=seed*1664525u+1013904223u;max[i]=(uint8_t)(seed>>24);}
        assert(!ku_copyout(&a,512,max,127));w=(ks_text_wire){0,0,1,0xffffffff,512,1+seed%127};
        sa.text_left=1024;sa.draw_left=64;calls_before=text_calls;
        int result=text_send(w);
        if(result<0)assert(text_calls==calls_before && sa.draw_left==64 && sa.text_left==1024);
        else assert(result==(int)w.length && text_calls==calls_before+1 && sa.draw_left==63 && sa.text_left==1024-w.length);
    }
    ks_request r=request(KS_TEXT,256,sizeof(w),0,0);r.token=sa.event.id+1;assert(submit(&sa,&a,r)==KS_STALE);
    r.token=sa.event.id;r.b--;assert(submit(&sa,&a,r)==KS_INVALID);r.b++;r.c=1;assert(submit(&sa,&a,r)==KS_INVALID);
    ks_request done=request(KS_COMPLETE,0,0,0,0);done.token=sa.event.id;assert(!submit(&sa,&a,done));
    assert(text_send(w)==KS_STALE);
    assert(!ks_post(&sa,KS_EVENT_TICK,0,0,0,0,0));assert(text_send(w)==KS_STALE);
    ks_destroy(&sa);assert(!ku_unpin(&a) && !ku_destroy(&a) && !live);
    printf("PASS %s optional TEXT/status/environment capabilities, bounded UTF-8 snapshot/control/overflow, atomic quota faults, token/generation, 5000 text mutations\n",arch?"x64":"i386");
}
static ks_line painted_line;static unsigned line_calls,line_width,line_height;
static void line_sink(void *ctx,const ks_line *l) {
    (void)ctx;assert(l->x0>=0 && l->x0<(int32_t)line_width && l->x1>=0 && l->x1<(int32_t)line_width);
    assert(l->y0>=0 && l->y0<(int32_t)line_height && l->y1>=0 && l->y1<(int32_t)line_height && !l->reserved);
    painted_line=*l;line_calls++;
}
static int line_send(ks_line l) {
    assert(!ku_copyout(&a,256,&l,sizeof(l)));
    ks_request r=request(KS_LINE,256,sizeof(l),0,0);r.token=sa.event.id;return submit(&sa,&a,r);
}
static void line_list_suite(int arch) {
    uint32_t root=setup(arch);memset(&sa,0,sizeof(sa));line_calls=0;line_width=100;line_height=80;
    assert(!ku_create(&a,&ops,root,arch,arch));
    assert(!ku_map(&a,0,KU_R|KU_W) && !ku_map(&a,4096,KU_R) && !ku_map(&a,8192,KU_R|KU_W) && !ku_pin(&a));
    assert(!ks_init(&sa,&a,11,100,80,sink,NULL));
    ks_line l={-20,40,120,40,0xffabcdef,0};assert(line_send(l)==KS_STALE);
    assert(!ks_post(&sa,KS_EVENT_DRAW,0,0,0,0,0));
    assert(submit(&sa,&a,request(KS_WAIT,8192,sizeof(ks_event),0,0))==sizeof(ks_event));
    assert(line_send(l)==KS_DENIED && sa.draw_left==64);sa.line_sink=line_sink;
    assert(submit(&sa,&a,request(KS_QUERY,0,0,0,0))==(KS_CAP_RECT|KS_CAP_INFO|KS_CAP_STATUS|KS_CAP_LINE|KS_CAP_LIST|KS_CAP_CLOCK));
    assert(line_send(l)==1 && painted_line.x0==0 && painted_line.x1==99 && painted_line.y0==40 && painted_line.color==0xabcdef);
    l=(ks_line){120,40,-20,40,1,0};assert(line_send(l)==1 && painted_line.x0==99 && painted_line.x1==0);
    l=(ks_line){30,-32768,30,32767,1,0};assert(line_send(l)==1 && painted_line.y0==0 && painted_line.y1==79);
    l=(ks_line){-32768,-32768,32767,32767,1,0};assert(line_send(l)==1 && painted_line.x0==0 && painted_line.y0==0 && painted_line.x1==79 && painted_line.y1==79);
    l=(ks_line){4,5,4,5,1,0};assert(line_send(l)==1 && painted_line.x0==4 && painted_line.y1==5);
    unsigned calls_before=line_calls,quota=sa.draw_left;
    l.x0=-2147483647-1;assert(line_send(l)==KS_INVALID && sa.draw_left==quota && line_calls==calls_before);
    l=(ks_line){-10,-1,110,-1,1,0};assert(line_send(l)==0 && line_calls==calls_before && sa.draw_left==quota-1);
    l=(ks_line){4,5,4,5,1,1};assert(line_send(l)==KS_INVALID);
    l.reserved=0;sa.draw_left=0;assert(line_send(l)==KS_QUOTA && line_calls==calls_before);
    sa.draw_left=64;ks_request r=request(KS_LINE,KU_SIZE-8,24,0,0);r.token=sa.event.id;
    assert(submit(&sa,&a,r)==KS_FAULT && sa.draw_left==64);
    r=request(KS_LINE,256,23,0,0);r.token=sa.event.id;assert(submit(&sa,&a,r)==KS_INVALID);
    uint32_t seed=981;
    for(unsigned i=0;i<10000;i++) {
        sa.width=line_width=1+((seed>>16)&4095);sa.height=line_height=1+(seed&4095);
        int32_t xy[4];for(unsigned k=0;k<4;k++){seed=seed*1664525u+1013904223u;xy[k]=(int32_t)(seed>>16)-32768;}
        l=(ks_line){xy[0],xy[1],xy[2],xy[3],seed,0};sa.draw_left=64;calls_before=line_calls;
        int result=line_send(l);assert(result==0 || result==1);
        assert(sa.draw_left==63 && line_calls==calls_before+(unsigned)result);
    }
    /* LIST cannot enumerate global files, and both outputs are atomic on fault. */
    assert(submit(&sa,&a,request(KS_LIST,0,8192,8256,0))==KS_MISSING);
    text_in(&a,512,"only.txt");int h=submit(&sa,&a,request(KS_OPEN,512,9,7,0));assert(h>0);
    text_in(&a,600,"xyz");assert(submit(&sa,&a,request(KS_WRITE,(unsigned)h,600,3,0))==3);
    memset(out,0xa5,64);assert(!ku_copyout(&a,8192,out,64));
    assert(submit(&sa,&a,request(KS_LIST,0,8192,4096,0))==KS_FAULT);
    assert(!ku_copyin(&a,out,8192,40));for(unsigned i=0;i<40;i++)assert(out[i]==0xa5);
    assert(submit(&sa,&a,request(KS_LIST,0,8192,8196,0))==KS_INVALID);
    assert(submit(&sa,&a,request(KS_LIST,0,KU_SIZE-20,8256,0))==KS_FAULT);
    assert(!submit(&sa,&a,request(KS_LIST,0,8192,8256,0)));
    char name[40];uint32_t size;
    assert(!ku_copyin(&a,name,8192,40) && !strcmp(name,"only.txt"));
    for(unsigned i=9;i<40;i++)assert(name[i]==0);
    assert(!ku_copyin(&a,&size,8256,4) && size==3);
    assert(submit(&sa,&a,request(KS_LIST,1,8192,8256,0))==KS_MISSING);
    assert(submit(&sa,&a,request(KS_LIST,0xffffffff,8192,8256,0))==KS_MISSING);
    assert(submit(&sa,&a,request(KS_LIST,0,8192,8256,1))==KS_INVALID);
    assert(!submit(&sa,&a,request(KS_CLOSE,(unsigned)h,0,0,0)));
    assert(!submit(&sa,&a,request(KS_DELETE,512,9,0,0)));
    assert(submit(&sa,&a,request(KS_LIST,0,8192,8256,0))==KS_MISSING);
    r=request(KS_COMPLETE,0,0,0,0);r.token=sa.event.id;assert(!submit(&sa,&a,r));assert(line_send(l)==KS_STALE);
    ks_destroy(&sa);assert(!ku_unpin(&a) && !ku_destroy(&a) && !live);
    printf("PASS %s LINE clipping/16bit/quota/phase/10000 random segments; LIST private namespace/dual-output atomic faults\n",arch?"x64":"i386");
}
static void clock_suite(int arch) {
    uint32_t root=setup(arch);memset(&sa,0,sizeof(sa));
    assert(!ku_create(&a,&ops,root,arch,arch));
    assert(!ku_map(&a,0,KU_R|KU_W) && !ku_map(&a,4096,KU_R) && !ku_map(&a,8192,KU_R|KU_W) && !ku_pin(&a));
    assert(!ks_init(&sa,&a,11,100,80,NULL,NULL));
    uint32_t sent[5]={77,88,99,111,222},got[5];assert(!ku_copyout(&a,8192,sent,20));
    ks_request r=request(KS_CLOCK,8192,20,0,0);
    assert((submit(&sa,&a,request(KS_QUERY,0,0,0,0))&KS_CAP_CLOCK)!=0);
    assert(submit(&sa,&a,r)==KS_AGAIN);
    assert(!ku_copyin(&a,got,8192,20) && !memcmp(got,sent,20));
    sa.clock_valid=1;sa.clock[0]=23;sa.clock[1]=59;sa.clock[2]=58;
    assert(submit(&sa,&a,r)==20);
    assert(!ku_copyin(&a,got,8192,20) && got[0]==1 && got[1]==20 && got[2]==23 && got[3]==59 && got[4]==58);
    r.b=19;assert(submit(&sa,&a,r)==KS_INVALID);r.b=20;
    r.c=1;assert(submit(&sa,&a,r)==KS_INVALID);r.c=0;
    r.d=1;assert(submit(&sa,&a,r)==KS_INVALID);r.d=0;
    r.token=1;assert(submit(&sa,&a,r)==KS_INVALID);r.token=0;
    r.generation++;assert(submit(&sa,&a,r)==KS_DENIED);r.generation--;
    r.a=4096;assert(submit(&sa,&a,r)==KS_FAULT);
    r.a=KU_SIZE-10;assert(submit(&sa,&a,r)==KS_FAULT);
    r.a=8192+4096-10;assert(!ku_copyout(&a,r.a,sent,10));
    assert(submit(&sa,&a,r)==KS_FAULT && !ku_copyin(&a,got,r.a,10) && !memcmp(got,sent,10));
    r.a=8192;
    for(unsigned field=0;field<3;field++) {
        uint32_t old=sa.clock[field];sa.clock[field]=field?60:24;
        assert(submit(&sa,&a,r)==KS_AGAIN);sa.clock[field]=old;
    }
    sa.clock[0]=sa.clock[1]=sa.clock[2]=0;assert(submit(&sa,&a,r)==20);
    assert(!ks_post(&sa,KS_EVENT_TICK,0,0,0,0,0) && submit(&sa,&a,r)==20);
    r.a=0;assert(submit(&sa,&a,r)==20); /* Request snapshot permits overlapping own output. */
    assert(!ku_copyin(&a,got,0,20) && got[0]==1 && got[1]==20 && !got[2] && !got[3] && !got[4]);
    ks_destroy(&sa);assert(!ku_unpin(&a) && !ku_destroy(&a) && !live);
    printf("PASS %s readonly CLOCK unavailable/ranges/phase/generation/token/atomic page faults/request snapshot\n",arch?"x64":"i386");
}
int main(void) {service_suite(0);service_suite(1);text_suite(0);text_suite(1);line_list_suite(0);line_list_suite(1);clock_suite(0);clock_suite(1);return 0;}
