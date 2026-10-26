#ifndef KV_USER_IMAGE_H
#define KV_USER_IMAGE_H
#include "user_space.h"
#define KI_HEADER 96u
#define KI_PAYLOAD_MAX 65536u
#define KI_BOOT_OFF (KU_SIZE-0x20000u)
#define KI_BOOT_VA (KU_BASE+KI_BOOT_OFF)
/* KAPP format2 / experimental ABI4. Prefix matches the 64-byte format1 header.
   Extension is eight u32s: text bytes, data VA offset, data file offset,
   data bytes, BSS bytes, stack bytes, protocol=1 (commands) or 2 (shared surface), reserved=0. */
typedef struct {
    uint32_t entry,text_bytes,data_va,data_file,data_bytes,bss_bytes,stack_bytes;
    char title[32];
    uint32_t protocol;
} ki_plan;
int ki_validate(const void *image,uint32_t bytes,int arch64,ki_plan *);
int ki_load(ku_space *,const ku_ops *,uint32_t kernel_root,const void *image,uint32_t bytes,int arch64,int nx,ki_plan *);
#endif
