#ifndef KV_USER_SPACE_H
#define KV_USER_SPACE_H
#include <stdint.h>
#define KU_BASE 0x00e00000u
#define KU_SIZE 0x00200000u
#define KU_PAGE 4096u
#define KU_PAGES 512u
#define KU_DIRECT_LIMIT 0x08000000u
#define KU_R 1u
#define KU_W 2u
#define KU_X 4u
/* Trusted allocator contract: fresh, reserved, direct-mapped frames below 128MiB.
   Caller serializes lifetime/mapping changes. This API does not switch CR3. */
typedef struct {
    uint32_t (*alloc)(void *ctx);
    void (*release)(void *ctx,uint32_t frame);
    void *(*direct)(void *ctx,uint32_t frame);
    void *ctx;
} ku_ops;
typedef struct {
    ku_ops ops;
    uint32_t root,tables[4],borrowed[3],frames[KU_PAGES];
    uint8_t rights[KU_PAGES];
    unsigned table_count,borrowed_count,pins;
    int live,arch64,nx;
} ku_space;
/* Start from a zero-initialized space. Failure rolls back owned frames only. */
int ku_create(ku_space *,const ku_ops *,uint32_t kernel_root,int arch64,int nx);
/* Refresh supervisor aliases while pinned, with IRQs off and kernel CR3 active.
   Borrowed spine identity must remain stable; private user mappings untouched. */
int ku_refresh(ku_space *,uint32_t kernel_root);
int ku_destroy(ku_space *); /* refuses pinned space; idempotent after teardown */
int ku_pin(ku_space *);
int ku_unpin(ku_space *);
int ku_map(ku_space *,uint32_t offset,unsigned rights); /* fresh zeroed page */
int ku_check(const ku_space *,uint32_t offset,uint32_t length,unsigned rights);
int ku_copyin(const ku_space *,void *kernel,uint32_t offset,uint32_t length);
int ku_copyout(const ku_space *,uint32_t offset,const void *kernel,uint32_t length);
int ku_load(ku_space *,uint32_t offset,const void *kernel,uint32_t length); /* offline RX loader */
#endif
