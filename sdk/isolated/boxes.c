#include "kvuser.h"
static kvu_event event;
static kvu_rect rects[3];
static uint32_t counter,stored;
static volatile uint32_t clicks,draws,delayed,delayed_draws;
static volatile int32_t draw_result,text_result,status_result;
static uint32_t env[4];
static volatile uint32_t info_hz,info_lang;
static const char private_status[]="Private RAM / Статус";
static kvu_text text;
static const char label[]="CPL3: KvantOS / Привет";
static const char filename[]="private.txt";
/* No kernel function pointers, libc, FPU or privileged instructions. */
void _start(const kvu_boot *boot) __attribute__((noreturn));
void _start(const kvu_boot *boot) {
    if(!boot || boot->version!=1 || boot->bytes!=32)kvu_exit(10);
    kvu_request q=kvu_req(boot,KVU_QUERY,0,0,0,0);
    int caps=kvu_call(&q);if(caps<0 || (caps&15)!=15)kvu_exit(16);
    q=kvu_req(boot,KVU_INFO,kvu_offset(env),sizeof(env),0,0);
    if(kvu_call(&q)!=16 || env[0]!=1 || env[1]!=16 || !env[2] || env[3]>1)kvu_exit(18);
    info_hz=env[2];info_lang=env[3];
    q=kvu_req(boot,KVU_STATUS,kvu_offset(private_status),sizeof(private_status)-1,0,0);
    status_result=kvu_call(&q);if(status_result)kvu_exit(19);
    q=kvu_req(boot,KVU_OPEN,kvu_offset(filename),sizeof(filename),7,0);
    int fd=kvu_call(&q);if(fd<0)kvu_exit(11);
    counter=42;q=kvu_req(boot,KVU_WRITE,(uint32_t)fd,kvu_offset(&counter),4,0);
    if(kvu_call(&q)!=4)kvu_exit(12);
    q=kvu_req(boot,KVU_READ,(uint32_t)fd,kvu_offset(&stored),4,0);
    if(kvu_call(&q)!=4 || stored!=42)kvu_exit(13);
    for(;;) {
        q=kvu_req(boot,KVU_WAIT,kvu_offset(&event),sizeof(event),0,0);
        if(kvu_call(&q)<0)continue;
        if(event.type==4)counter+=event.delta_ms;
        if(event.type==2 || event.type==3)counter+=30;
        if(event.type==3)clicks++;
        if(event.type==2 && event.key=='p')delayed=1;
        if(event.type==1) {
            draws++;
            if(delayed) {
                /* Deliberately span a quantum: DRAW may resume in a tick pump. */
                uint32_t start=(uint32_t)kvu_gate(3,0,0);
                while((uint32_t)((uint32_t)kvu_gate(3,0,0)-start)<8){}
                delayed=0;delayed_draws++;
            }
            rects[0]=(kvu_rect){1,0x111b2b,0,0,(int32_t)event.width,(int32_t)event.height};
            rects[1]=(kvu_rect){1,0x32c78a,20+(int32_t)(counter%200),60,64,64};
            rects[2]=(kvu_rect){2,0x71a6ff,10,10,(int32_t)event.width-20,(int32_t)event.height-20};
            q=kvu_req(boot,KVU_DRAW,kvu_offset(rects),3,0,0);q.token=event.id;
            draw_result=kvu_call(&q);if(draw_result<0)kvu_exit(14);
            text=(kvu_text){16,30,0xf1d257,0xffffffff,kvu_offset(label),sizeof(label)-1};
            q=kvu_req(boot,KVU_TEXT,kvu_offset(&text),sizeof(text),0,0);q.token=event.id;
            text_result=kvu_call(&q);if(text_result!=(int32_t)(sizeof(label)-1))kvu_exit(17);
        }
        q=kvu_req(boot,KVU_COMPLETE,0,0,0,0);q.token=event.id;
        if(kvu_call(&q)<0)kvu_exit(15);
        if(event.type==5)kvu_exit(0);
    }
}
