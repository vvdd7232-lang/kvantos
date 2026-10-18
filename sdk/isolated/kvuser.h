#ifndef KVUSER_H
#define KVUSER_H
#include <stdint.h>
#include <stddef.h>
/* Experimental ABI4 service protocol1. User pointers become arena offsets. */
typedef struct {uint32_t version,bytes,generation,width,height,arch64,nx,reserved;} kvu_boot;
typedef struct {uint32_t version,bytes,op,flags,generation,token,a,b,c,d;} kvu_request;
typedef struct {uint32_t version,bytes,generation,id,type,delta_ms,width,height;int32_t x,y,key,button;} kvu_event;
typedef struct {int32_t x,y;uint32_t color,bg,offset,length;} kvu_text;
typedef struct {int32_t x0,y0,x1,y1;uint32_t color,reserved;} kvu_line;
typedef struct {uint32_t type,color;int32_t x,y,w,h;} kvu_rect;
enum {KVU_WAIT=1,KVU_COMPLETE,KVU_DRAW,KVU_OPEN,KVU_READ,KVU_WRITE,KVU_CLOSE,KVU_DELETE,KVU_TEXT,KVU_QUERY,KVU_STATUS,KVU_INFO,KVU_LINE,KVU_LIST,KVU_CLOCK};
_Static_assert(sizeof(kvu_request)==40 && sizeof(kvu_event)==48 && sizeof(kvu_rect)==24 && sizeof(kvu_text)==24 && sizeof(kvu_line)==24,"wire protocol");
static inline uint32_t kvu_offset(const void *p) {return (uint32_t)((uintptr_t)p-0x00e00000u);}
static inline int32_t kvu_gate(uint32_t id,uint32_t offset,uint32_t bytes) {
    uintptr_t a=id,b=offset,c=bytes;
    __asm__ volatile("int $0x80":"+a"(a):"b"(b),"c"(c):"memory","cc");return (int32_t)a;
}
static inline int32_t kvu_call(kvu_request *r) {return kvu_gate(2,kvu_offset(r),sizeof(*r));}
static inline void kvu_exit(uint32_t code) __attribute__((noreturn));
static inline void kvu_exit(uint32_t code) {kvu_gate(0,code,0);for(;;){} }
static inline kvu_request kvu_req(const kvu_boot *b,uint32_t op,uint32_t a,uint32_t c,uint32_t d,uint32_t e) {
    return (kvu_request){1,40,op,0,b->generation,0,a,c,d,e};
}
#endif
