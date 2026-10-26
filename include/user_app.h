#ifndef KV_USER_APP_H
#define KV_USER_APP_H
#include "user_service.h"
int ku_app_load(const void *image,uint32_t bytes,ks_sink,ks_text_sink,ks_line_sink,void *);
void ku_app_unload(void);
/* GUI-thread-only borrowed access: do not pump or unload during a blit. */
int ku_app_surface_size(uint32_t *width,uint32_t *height);
int ku_app_surface_pixel(uint32_t x,uint32_t y,uint32_t *argb);
int ku_app_live(void);
/* Kernel-side sink context only; never exposed as a user function pointer. */
uint32_t ku_app_draw_token(void);
const char *ku_app_error(void);
const char *ku_app_status(void);
int ku_app_event(uint32_t type,uint32_t delta,uint32_t width,uint32_t height,int32_t x,int32_t y,int32_t key,int32_t button);
#endif
