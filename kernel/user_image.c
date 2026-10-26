/* Strict bounded format2 loader. No fallbacks or user-selected physical frames. */
#include "user_image.h"
#ifdef KU_HOSTTEST
#include <string.h>
#else
#include "kernel.h"
#endif
static uint32_t u32at(const uint8_t *p) {return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static unsigned u16at(const uint8_t *p) {return p[0]|((unsigned)p[1]<<8);}
int ki_validate(const void *image,uint32_t bytes,int arch64,ki_plan *out) {
    if(!image || !out || (arch64!=0 && arch64!=1) || bytes<KI_HEADER || bytes>KI_HEADER+KI_PAYLOAD_MAX)return -1;
    const uint8_t *b=image;ki_plan p;
    if(memcmp(b,"KAPP",4) || u16at(b+4)!=2 || u16at(b+6)!=KI_HEADER || u32at(b+8)!=4 ||
       u32at(b+12)!=(1u|(arch64?256u:0u)) || u32at(b+16)!=KU_BASE || u32at(b+24)!=bytes-KI_HEADER ||
       (u32at(b+88)!=1 && u32at(b+88)!=2) || u32at(b+92))return -1;
    p.entry=u32at(b+20);p.text_bytes=u32at(b+64);p.data_va=u32at(b+68);p.data_file=u32at(b+72);
    p.data_bytes=u32at(b+76);p.bss_bytes=u32at(b+80);p.stack_bytes=u32at(b+84);
    uint32_t payload=bytes-KI_HEADER;
    if(!p.text_bytes || p.text_bytes>payload || p.entry<KU_BASE || p.entry-KU_BASE>=p.text_bytes ||
       (p.data_va&4095) || p.data_va<((p.text_bytes+4095)&~4095u) || p.data_va>KI_BOOT_OFF ||
       p.data_bytes>KI_BOOT_OFF-p.data_va || p.bss_bytes>KI_BOOT_OFF-p.data_va-p.data_bytes ||
       p.bss_bytes!=u32at(b+28) || p.data_file<p.text_bytes || (p.data_file&15) ||
       p.data_file>payload || p.data_bytes!=payload-p.data_file ||
       p.stack_bytes<4096 || p.stack_bytes>65536 || (p.stack_bytes&4095))return -1;
    int terminated=0;for(unsigned i=0;i<32;i++)if(!b[32+i])terminated=1;
    if(!terminated)return -1;
    for(uint32_t i=p.text_bytes;i<p.data_file;i++)if(b[KI_HEADER+i])return -1;
    p.protocol=u32at(b+88);memcpy(p.title,b+32,32);*out=p;return 0;
}
int ki_load(ku_space *space,const ku_ops *ops,uint32_t root,const void *image,uint32_t bytes,int arch64,int nx,ki_plan *out) {
    ki_plan p;if(!out || ki_validate(image,bytes,arch64,&p))return -1;
    if(ku_create(space,ops,root,arch64,nx))return -1;
    for(uint32_t off=0;off<p.text_bytes;off+=4096)if(ku_map(space,off,KU_R|KU_X))goto fail;
    for(uint32_t n=0;n<p.data_bytes+p.bss_bytes;n+=4096)if(ku_map(space,p.data_va+n,KU_R|KU_W))goto fail;
    for(uint32_t off=KU_SIZE-p.stack_bytes;off<KU_SIZE;off+=4096)if(ku_map(space,off,KU_R|KU_W))goto fail;
    if(ku_map(space,KI_BOOT_OFF,KU_R))goto fail;
    const uint8_t *b=image;
    if(ku_load(space,0,b+KI_HEADER,p.text_bytes) || ku_load(space,p.data_va,b+KI_HEADER+p.data_file,p.data_bytes))goto fail;
    *out=p;return 0;
fail:
    ku_destroy(space);return -1;
}
