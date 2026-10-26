#ifndef KV_USER_SURFACE_H
#define KV_USER_SURFACE_H
#include "user_space.h"
/* Two mapped ARGB8888 (straight alpha) buffers. No user/kernel VA on wire. */
typedef struct {uint32_t version,bytes,generation,width,height,stride,offset[2];} ku_surface_info;
typedef struct {
    ku_shared buffer[2];
    ku_space *space;
    ku_surface_info info;
    uint32_t sequence;
    unsigned front;
    int live,presented,pending; /* pending = index+1, applied at event boundary */
} ku_surface;
int ku_surface_create(ku_surface *,ku_space *,uint32_t generation,uint32_t width,uint32_t height);
int ku_surface_destroy(ku_surface *);
int ku_surface_queue(ku_surface *,unsigned back,uint32_t sequence);
/* Only offline, unpinned, kernel CR3 active. Reload user CR3 before resuming. */
int ku_surface_commit(ku_surface *);
int ku_surface_pixel(const ku_surface *,uint32_t x,uint32_t y,uint32_t *argb);
#endif
