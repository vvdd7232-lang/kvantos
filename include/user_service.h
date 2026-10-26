#ifndef KV_USER_SERVICE_H
#define KV_USER_SERVICE_H
#include "user_space.h"
#include "user_surface.h"
#define KS_VERSION 1u
#define KS_SURFACE_VERSION 2u
#define KS_FILES 8u
#define KS_FILE_BYTES 1024u
#define KS_HANDLES 8u
#define KS_COMMANDS 32u
#define KS_EVENT_DRAW 1u
#define KS_EVENT_KEY 2u
#define KS_EVENT_CLICK 3u
#define KS_EVENT_TICK 4u
#define KS_EVENT_CLOSE 5u
#define KS_OPEN_R 1u
#define KS_OPEN_W 2u
#define KS_OPEN_CREATE 4u
#define KS_OPEN_TRUNC 8u
enum {KS_WAIT=1,KS_COMPLETE,KS_DRAW,KS_OPEN,KS_READ,KS_WRITE,KS_CLOSE,KS_DELETE,KS_TEXT,KS_QUERY,KS_STATUS,KS_INFO,KS_LINE,KS_LIST,KS_CLOCK,KS_SURFACE_INFO,KS_SURFACE_PRESENT};
#define KS_CAP_RECT 1u
#define KS_CAP_TEXT 2u
#define KS_CAP_STATUS 4u
#define KS_CAP_INFO 8u
#define KS_CAP_LINE 16u
#define KS_CAP_LIST 32u
#define KS_CAP_CLOCK 64u
#define KS_CAP_SURFACE 128u
#define KS_TEXT_BYTES 127u
#define KS_TEXT_BUDGET 1024u
enum {KS_INVALID=-1,KS_FAULT=-2,KS_UNKNOWN=-3,KS_DENIED=-4,KS_QUOTA=-5,KS_STALE=-6,KS_BUSY=-7,KS_AGAIN=-8,KS_MISSING=-9};
/* Internal draft service protocol. Not a frozen public KAPP ABI4. No native pointers. */
typedef struct {uint32_t version,bytes,op,flags,generation,token,a,b,c,d;} ks_request;
typedef struct {uint32_t version,bytes,generation,id,type,delta_ms,width,height;int32_t x,y,key,button;} ks_event;
typedef struct {uint32_t type,color;int32_t x,y,w,h;} ks_rect;
/* TEXT wire: position/colors and offset+byte count (no NUL on wire). */
typedef struct {int32_t x,y;uint32_t color,bg,offset,length;} ks_text_wire;
typedef struct {int32_t x,y;uint32_t color,bg;char bytes[128];} ks_text;
typedef struct {int32_t x0,y0,x1,y1;uint32_t color,reserved;} ks_line;
typedef void (*ks_line_sink)(void *,const ks_line *);
typedef void (*ks_text_sink)(void *,const ks_text *);
typedef void (*ks_sink)(void *,const ks_rect *,unsigned);
typedef struct {
    const ku_space *space;
    ks_sink sink;void *sink_ctx;
    uint32_t generation,next_handle,next_event,width,height,draw_left;
    int live,pending,delivered;
    ks_event event;
    struct {char name[32];uint32_t size,refs;int used;uint8_t data[KS_FILE_BYTES];} files[KS_FILES];
    struct {uint32_t cookie,file,rights;} handles[KS_HANDLES];
    uint8_t scratch[4096];ks_rect rects[KS_COMMANDS];
    ks_text_sink text_sink;uint32_t text_left,hz,language;char status[96];ks_line_sink line_sink;
    uint32_t clock[3];int clock_valid;
    /* Append-only internal fields: keep diagnostic prefix offsets stable. */
    uint32_t protocol;ku_surface *surface;
} ks_service;
int ks_init(ks_service *,const ku_space *,uint32_t generation,uint32_t width,uint32_t height,ks_sink,void *);
void ks_destroy(ks_service *);
int ks_post(ks_service *,uint32_t type,uint32_t delta_ms,int32_t x,int32_t y,int32_t key,int32_t button);
int32_t ks_dispatch(ks_service *,uint32_t request_offset,uint32_t request_size);
#endif
