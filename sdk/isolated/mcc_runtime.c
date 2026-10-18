/* User-only adapter for an opt-in MCC target. The API3-shaped table below
   contains ONLY private user-code addresses, never kernel function pointers.
   This file is prelinked at build time; Workshop still compiles source itself. */
#include "kvuser.h"
#include "kvapp.h"
#include <stdarg.h>
static const kvu_boot *boot;
static kvu_event event;
static uint32_t environment[4],seed;
static unsigned char heap[32768] __attribute__((aligned(8)));
static int heap_ready;
typedef struct {uint32_t bytes,free;} block;
void *memcpy(void *d,const void *s,size_t n) {unsigned char *p=d;const unsigned char *q=s;while(n--)*p++=*q++;return d;}
void *memset(void *d,int c,size_t n) {unsigned char *p=d;while(n--)*p++=(unsigned char)c;return d;}
static int service(uint32_t op,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t token) {
    kvu_request r=kvu_req(boot,op,a,b,c,d);r.token=token;return kvu_call(&r);
}
static kv_u32 length(const char *s) {kv_u32 n=0;if(s)while(s[n])n++;return n;}
static kv_u32 prefix(const char *s,kv_u32 max) {
    if(!s)return 0;
    kv_u32 n=0;while(n<max && s[n])n++;
    if(n==max)while(n && ((unsigned char)s[n]&0xc0)==0x80)n--;
    return n;
}
static kv_i32 width(void) {return (kv_i32)event.width;}
static kv_i32 height(void) {return (kv_i32)event.height;}
static kv_u32 rgb(kv_u8 r,kv_u8 g,kv_u8 b) {return ((kv_u32)r<<16)|((kv_u32)g<<8)|b;}
static void rectangle(int type,int x,int y,int w,int h,kv_u32 color) {
    if(w<=0 || h<=0)return;
    kvu_rect r={(uint32_t)type,color,x,y,w,h};
    if(service(KVU_DRAW,kvu_offset(&r),1,0,0,event.id)<0)kvu_exit(31);
}
static void clear(kv_u32 color) {rectangle(1,0,0,width(),height(),color);}
static void pixel(kv_i32 x,kv_i32 y,kv_u32 color) {rectangle(1,x,y,1,1,color);}
static void fill(kv_i32 x,kv_i32 y,kv_i32 w,kv_i32 h,kv_u32 c) {rectangle(1,x,y,w,h,c);}
static void rect(kv_i32 x,kv_i32 y,kv_i32 w,kv_i32 h,kv_u32 c) {rectangle(2,x,y,w,h,c);}
static void text(kv_i32 x,kv_i32 y,const char *s,kv_u32 fg,kv_u32 bg) {
    kv_u32 n=prefix(s,127);if(!n)return;
    kvu_text t={x,y,fg,bg,kvu_offset(s),n};
    if(service(KVU_TEXT,kvu_offset(&t),sizeof(t),0,0,event.id)<0)kvu_exit(32);
}
static kv_i32 text_width(const char *s) {kv_u32 n=0;if(s)while(*s)if(((unsigned char)*s++&0xc0)!=0x80)n++;return (kv_i32)(n*8);}
static void status(const char *s) {kv_u32 n=prefix(s,95);if(service(KVU_STATUS,n?kvu_offset(s):0,n,0,0,0)<0)kvu_exit(33);}
static void *allocate(kv_u32 n) {
    if(!n || n>sizeof(heap)-sizeof(block))return NULL;
    n=(n+7)&~7u;
    if(!heap_ready) {block *b=(block *)heap;b->bytes=sizeof(heap)-sizeof(block);b->free=1;heap_ready=1;}
    for(block *b=(block *)heap;(unsigned char *)b<heap+sizeof(heap);b=(block *)((unsigned char *)(b+1)+b->bytes)) {
        if(!b->free || b->bytes<n)continue;
        if(b->bytes-n>=sizeof(block)+8) {
            block *next=(block *)((unsigned char *)(b+1)+n);next->bytes=b->bytes-n-sizeof(block);next->free=1;b->bytes=n;
        }
        b->free=0;return b+1;
    }
    return NULL;
}
static void release(void *p) {
    if(!p || !heap_ready)return;
    for(block *b=(block *)heap;(unsigned char *)b<heap+sizeof(heap);b=(block *)((unsigned char *)(b+1)+b->bytes))if(b+1==p) {b->free=1;break;}
    for(block *b=(block *)heap;(unsigned char *)b<heap+sizeof(heap);) {
        block *next=(block *)((unsigned char *)(b+1)+b->bytes);
        if((unsigned char *)next>=heap+sizeof(heap))break;
        if(b->free && next->free)b->bytes+=sizeof(block)+next->bytes;else b=next;
    }
}
static kv_u32 ticks(void) {return (kv_u32)kvu_gate(3,0,0);}
static kv_u32 hz(void) {return environment[2];}
static kv_u32 seconds(void) {return ticks()/hz();}
static kv_u32 lang(void) {
    (void)service(KVU_INFO,kvu_offset(environment),sizeof(environment),0,0,0);return environment[3];
}
static kv_u32 delta_ms(void) {return event.type==4?event.delta_ms:0;}
static int open_file(const char *name,unsigned flags) {
    if(!name)return -1;
    kv_u32 n=length(name);if(n>31)return -1;
    return service(KVU_OPEN,kvu_offset(name),n+1,flags,0,0);
}
static kv_i32 file_read(const char *name,void *out,kv_u32 max) {
    int fd=open_file(name,1);if(fd<0)return fd;if(max>1024)max=1024;
    int n=service(KVU_READ,(uint32_t)fd,kvu_offset(out),max,0,0);
    (void)service(KVU_CLOSE,(uint32_t)fd,0,0,0,0);return n;
}
static kv_i32 file_write(const char *name,const void *data,kv_u32 size) {
    if(size>1024)return -5;
    int fd=open_file(name,15);if(fd<0)return fd;
    int n=service(KVU_WRITE,(uint32_t)fd,kvu_offset(data),size,0,0);
    (void)service(KVU_CLOSE,(uint32_t)fd,0,0,0,0);return n;
}
static kv_i32 file_delete(const char *name) {
    if(!name)return -1;
    kv_u32 n=length(name);if(n>31)return -1;
    return service(KVU_DELETE,kvu_offset(name),n+1,0,0,0);
}
static kv_i32 file_exists(const char *name) {int fd=open_file(name,1);if(fd<0)return 0;(void)service(KVU_CLOSE,(uint32_t)fd,0,0,0,0);return 1;}
static void *mem_set(void *d,int c,kv_u32 n) {return memset(d,c,n);}
static void *mem_copy(void *d,const void *s,kv_u32 n) {return memcpy(d,s,n);}
static kv_i32 str_cmp(const char *a,const char *b) {while(*a && *a==*b) {a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
static void str_copy(char *d,const char *s,kv_u32 max) {if(!max)return;kv_u32 i=0;while(i+1<max && s[i]) {d[i]=s[i];i++;}d[i]=0;}
static void emit_char(char *d,kv_u32 size,kv_u32 *used,char c) {if(size && *used<size-1)d[(*used)++]=c;}
static void format(char *d,kv_u32 size,const char *fmt,...) {
    va_list args;va_start(args,fmt);kv_u32 used=0;
    while(*fmt) {
        if(*fmt!='%') {emit_char(d,size,&used,*fmt++);continue;}
        char spec=*++fmt;if(!spec)break;fmt++;
        if(spec=='s') {const char *s=va_arg(args,const char *);if(!s)s="(null)";while(*s)emit_char(d,size,&used,*s++);}
        else if(spec=='c')emit_char(d,size,&used,(char)va_arg(args,int));
        else if(spec=='%')emit_char(d,size,&used,'%');
        else if(spec=='d' || spec=='u' || spec=='x') {
            kv_u32 n=va_arg(args,kv_u32),base=spec=='x'?16:10;char number[16];unsigned at=0;
            if(spec=='d' && (kv_i32)n<0) {emit_char(d,size,&used,'-');n=0u-n;}
            do {number[at++]="0123456789abcdef"[n%base];n/=base;}while(n);
            while(at)emit_char(d,size,&used,number[--at]);
        } else {emit_char(d,size,&used,'%');emit_char(d,size,&used,spec);}
    }
    va_end(args);if(size)d[used]=0;
}
static kv_u32 random_value(void) {if(!seed)seed=boot->generation^ticks()^0x715ab31u;seed=seed*1664525u+1013904223u;return seed;}
/* Compiler rejects these names in the isolated profile. Defensive user-only
   stubs still terminate if a corrupt program reaches their table entries. */
static void draw_line(kv_i32 x0,kv_i32 y0,kv_i32 x1,kv_i32 y1,kv_u32 color) {
    kvu_line l={x0,y0,x1,y1,color,0};
    if(service(KVU_LINE,kvu_offset(&l),sizeof(l),0,0,event.id)<0)kvu_exit(34);
}
static kv_i32 file_list(kv_i32 index,char *name,kv_u32 *size) {
    char copy[40];kv_u32 bytes;
    int rc=service(KVU_LIST,(kv_u32)index,kvu_offset(copy),kvu_offset(&bytes),0,0);
    if(!rc) {if(name)memcpy(name,copy,40);if(size)*size=bytes;}return rc;
}
static void clock_value(kv_i32 *h,kv_i32 *m,kv_i32 *s) {
    uint32_t value[5];
    if(service(KVU_CLOCK,kvu_offset(value),sizeof(value),0,0,0)!=20 ||
       value[0]!=1 || value[1]!=20 || value[2]>23 || value[3]>59 || value[4]>59)kvu_exit(35);
    if(h)*h=(kv_i32)value[2];
    if(m)*m=(kv_i32)value[3];
    if(s)*s=(kv_i32)value[4];
}
static void unsupported_beep(kv_u32 a,kv_u32 b) {(void)a;(void)b;kvu_exit(27);}
static void unsupported_log(const char *s) {(void)s;kvu_exit(27);}
static const kv_api_t api={
    .api_version=3,.width=width,.height=height,.rgb=rgb,.clear=clear,.pixel=pixel,
    .fill=fill,.rect=rect,.line=draw_line,.text=text,.text_width=text_width,.status=status,
    .alloc=allocate,.release=release,.ticks=ticks,.hz=hz,.seconds=seconds,
    .clock=clock_value,.beep=unsupported_beep,.file_read=file_read,.file_write=file_write,
    .file_delete=file_delete,.file_list=file_list,.file_exists=file_exists,
    .mem_set=mem_set,.mem_copy=mem_copy,.str_len=length,.str_cmp=str_cmp,.str_copy=str_copy,
    .format=format,.random=random_value,.log=unsupported_log,.lang=lang,.delta_ms=delta_ms
};
void _start(const kvu_boot *info) __attribute__((noreturn));
void _start(const kvu_boot *info) {
    boot=info;if(!boot || boot->version!=1 || boot->bytes!=32)kvu_exit(20);
    int caps=service(KVU_QUERY,0,0,0,0,0);if(caps<0 || (caps&127)!=127)kvu_exit(21);
    if(service(KVU_INFO,kvu_offset(environment),sizeof(environment),0,0,0)!=16 ||
       environment[0]!=1 || environment[1]!=16 || !hz())kvu_exit(22);
    const kv_app_t *app=((const kv_app_t *(*)(const kv_api_t *))(uintptr_t)0x00e00000)(&api);
    if(!app)kvu_exit(23);
    int opened=0;
    for(;;) {
        if(service(KVU_WAIT,kvu_offset(&event),sizeof(event),0,0,0)<0)continue;
        if(event.type==1) {if(!opened) {opened=1;if(app->on_open)app->on_open();}if(app->on_draw)app->on_draw();}
        else if(event.type==2 && app->on_key)app->on_key(event.key);
        else if(event.type==3 && app->on_click)app->on_click(event.x,event.y,event.button);
        else if(event.type==4 && app->on_tick)app->on_tick();
        else if(event.type==5 && app->on_close)app->on_close();
        if(service(KVU_COMPLETE,0,0,0,0,event.id)<0)kvu_exit(24);
        if(event.type==5)kvu_exit(0);
    }
}
