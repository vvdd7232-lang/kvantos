/* M4 private address spaces. Kernel tables are borrowed supervisor-only;
   only the spine to the 14..16MiB arena is cloned. No CR3 switch here. */
#include "user_space.h"
#ifdef KU_HOSTTEST
#include <string.h>
#else
#include "kernel.h"
#endif
#define P 1ull
#define W 2ull
#define U 4ull
#define PS 128ull
#define NX (1ull<<63)
#define ADDRESS 0x000ffffffffff000ull
static int valid_frame(uint32_t p) {return p && !(p&4095u) && p<KU_DIRECT_LIMIT;}
static void *direct(const ku_space *s,uint32_t p) {
    return valid_frame(p)?s->ops.direct(s->ops.ctx,p):NULL;
}
static uint32_t address(uint64_t e) {
    uint64_t p=e&ADDRESS;return p<KU_DIRECT_LIMIT?(uint32_t)p:0;
}
static uint32_t new_frame(ku_space *s) {
    uint32_t p=s->ops.alloc(s->ops.ctx);
    if(!valid_frame(p)) {if(p)s->ops.release(s->ops.ctx,p);return 0;}
    for(unsigned i=0;i<s->borrowed_count;i++)if(s->borrowed[i]==p)return 0;
    for(unsigned i=0;i<s->table_count;i++)if(s->tables[i]==p)return 0;
    for(unsigned i=0;i<KU_PAGES;i++)if(s->frames[i]==p)return 0;
    void *v=direct(s,p);
    if(!v) {s->ops.release(s->ops.ctx,p);return 0;}
    memset(v,0,KU_PAGE);return p;
}
static int borrow(ku_space *s,uint32_t p) {
    if(!direct(s,p) || s->borrowed_count==3)return -1;
    for(unsigned i=0;i<s->borrowed_count;i++)if(s->borrowed[i]==p)return -1;
    s->borrowed[s->borrowed_count++]=p;return 0;
}
static int table(ku_space *s) {
    uint32_t p=new_frame(s);if(!p)return -1;
    s->tables[s->table_count++]=p;return 0;
}
int ku_destroy(ku_space *s) {
    if(!s || s->pins)return -1;
    if(!s->live)return 0;
    for(unsigned i=0;i<KU_PAGES;i++)if(s->frames[i]) {
        memset(direct(s,s->frames[i]),0,KU_PAGE);s->ops.release(s->ops.ctx,s->frames[i]);
    }
    for(unsigned i=0;i<s->table_count;i++) {
        memset(direct(s,s->tables[i]),0,KU_PAGE);s->ops.release(s->ops.ctx,s->tables[i]);
    }
    memset(s,0,sizeof(*s));return 0;
}
int ku_create(ku_space *s,const ku_ops *ops,uint32_t kernel_root,int arch64,int nx) {
    if(!s || !ops || !ops->alloc || !ops->release || !ops->direct || s->live ||
       (arch64!=0 && arch64!=1) || (nx!=0 && nx!=1) || (nx && !arch64))return -1;
    memset(s,0,sizeof(*s));s->ops=*ops;s->live=1;s->arch64=arch64;s->nx=nx;
    if(borrow(s,kernel_root))goto fail;
    if(arch64) {
        uint64_t *root=direct(s,kernel_root),e=root[0];
        if(!(e&P) || (e&PS) || borrow(s,address(e)))goto fail;
        uint64_t *pdpt=direct(s,address(e));e=pdpt[0];
        if(!(e&P) || (e&PS) || borrow(s,address(e)))goto fail;
        for(unsigned i=0;i<4;i++)if(table(s))goto fail;
        for(unsigned l=0;l<3;l++) {
            uint64_t *src=direct(s,s->borrowed[l]),*dst=direct(s,s->tables[l]);
            for(unsigned i=0;i<512;i++)dst[i]=src[i]&~U;
        }
        ((uint64_t *)direct(s,s->tables[0]))[0]=s->tables[1]|P|W|U;
        ((uint64_t *)direct(s,s->tables[1]))[0]=s->tables[2]|P|W|U;
        ((uint64_t *)direct(s,s->tables[2]))[KU_BASE>>21]=s->tables[3]|P|W|U;
    } else {
        uint32_t *root=direct(s,kernel_root),e=root[KU_BASE>>22];
        if(!(e&P) || (e&PS) || borrow(s,address(e)))goto fail;
        for(unsigned i=0;i<2;i++)if(table(s))goto fail;
        for(unsigned l=0;l<2;l++) {
            uint32_t *src=direct(s,s->borrowed[l]),*dst=direct(s,s->tables[l]);
            for(unsigned i=0;i<1024;i++)dst[i]=src[i]&~(uint32_t)U;
        }
        uint32_t *pt=direct(s,s->tables[1]);
        for(unsigned i=(KU_BASE>>12)&1023;i<1024;i++)pt[i]=0;
        ((uint32_t *)direct(s,s->tables[0]))[KU_BASE>>22]=s->tables[1]|(uint32_t)(P|W|U);
    }
    s->root=s->tables[0];return 0;
fail:
    ku_destroy(s);return -1;
}
/* Private upper tables outlive the GUI frame. A newly mapped MMIO range may
   add kernel root/PDPT/PD entries after create. Refresh only supervisor parts;
   fail closed if the borrowed spine was replaced, before touching any entry. */
int ku_refresh(ku_space *s,uint32_t kernel_root) {
    if(!s || !s->live || kernel_root!=s->borrowed[0])return -1;
    if(s->arch64) {
        if(s->borrowed_count!=3 || s->table_count!=4)return -1;
        uint64_t *src[3],*dst[3];
        for(unsigned l=0;l<3;l++) {
            src[l]=direct(s,s->borrowed[l]);dst[l]=direct(s,s->tables[l]);
            if(!src[l] || !dst[l])return -1;
        }
        for(unsigned l=0;l<2;l++)
            if(!(src[l][0]&P) || (src[l][0]&PS) || address(src[l][0])!=s->borrowed[l+1])return -1;
        for(unsigned l=0;l<3;l++)for(unsigned i=0;i<512;i++)
            if(i!=(l==2?KU_BASE>>21:0))dst[l][i]=src[l][i]&~U;
    } else {
        if(s->borrowed_count!=2 || s->table_count!=2)return -1;
        uint32_t *src=direct(s,s->borrowed[0]),*dst=direct(s,s->tables[0]);
        uint32_t *srcpt=direct(s,s->borrowed[1]),*dstpt=direct(s,s->tables[1]);
        if(!src || !dst || !srcpt || !dstpt || !(src[KU_BASE>>22]&P) ||
           (src[KU_BASE>>22]&PS) || address(src[KU_BASE>>22])!=s->borrowed[1])return -1;
        for(unsigned i=0;i<1024;i++)if(i!=KU_BASE>>22)dst[i]=src[i]&~(uint32_t)U;
        for(unsigned i=0;i<((KU_BASE>>12)&1023);i++)dstpt[i]=srcpt[i]&~(uint32_t)U;
    }
    return 0;
}
int ku_pin(ku_space *s) {
    if(!s || !s->live || s->pins==0xffffffffu)return -1;
    s->pins++;return 0;
}
int ku_unpin(ku_space *s) {
    if(!s || !s->live || !s->pins)return -1;
    s->pins--;return 0;
}
int ku_map(ku_space *s,uint32_t off,unsigned rights) {
    if(!s || !s->live || s->pins || off>=KU_SIZE || (off&4095) ||
       !(rights&KU_R) || (rights&~7u) || ((rights&(KU_W|KU_X))==(KU_W|KU_X)))return -1;
    unsigned i=off>>12;if(s->frames[i])return -1;
    uint32_t p=new_frame(s);if(!p)return -1;
    s->frames[i]=p;s->rights[i]=(uint8_t)rights;
    if(s->arch64) {
        uint64_t *pt=direct(s,s->tables[3]);
        pt[i]=p|P|U|((rights&KU_W)?W:0)|((s->nx && !(rights&KU_X))?NX:0);
    } else {
        uint32_t *pt=direct(s,s->tables[1]);
        pt[((KU_BASE+off)>>12)&1023]=p|(uint32_t)(P|U|((rights&KU_W)?W:0));
    }
    return 0;
}
/* Validate actual page walks, not just a metadata range. This catches stale
   U/S/RW, large-page aliases, changed physical frames and inherited NX. */
static uint32_t translate(const ku_space *s,uint32_t off,unsigned rights) {
    uint32_t virt=KU_BASE+off,frame=s->root;uint64_t e=0;
    for(int l=s->arch64?3:1;l>=0;l--) {
        void *v=direct(s,frame);if(!v)return 0;
        unsigned idx=((uint64_t)virt>>(12+(s->arch64?9:10)*l))&(s->arch64?511u:1023u);
        e=s->arch64?((uint64_t *)v)[idx]:((uint32_t *)v)[idx];
        if((e&(P|U))!=(P|U) || ((rights&KU_W) && !(e&W)) ||
           (s->arch64 && (e&NX) && (!s->nx || (rights&KU_X))) || (l && (e&PS)))return 0;
        frame=address(e);if(!frame)return 0;
    }
    return frame==s->frames[off>>12] && direct(s,frame)?frame:0;
}
int ku_check(const ku_space *s,uint32_t off,uint32_t length,unsigned rights) {
    if(!s || !s->live || !rights || (rights&~7u) || off>KU_SIZE || length>KU_SIZE-off)return -1;
    if(!length)return 0;
    unsigned first=off>>12,last=(off+length-1)>>12;
    for(unsigned i=first;i<=last;i++)
        if((s->rights[i]&rights)!=rights || !translate(s,i<<12,rights))return -1;
    return 0;
}
static int copy(const ku_space *s,uint32_t off,void *kernel,uint32_t length,int to_user,int loading) {
    if((length && !kernel) || ku_check(s,off,length,to_user && !loading?KU_R|KU_W:KU_R))return -1;
    uint8_t *k=kernel;
    while(length) {
        unsigned inside=off&4095,n=KU_PAGE-inside;if(n>length)n=length;
        uint8_t *u=(uint8_t *)direct(s,s->frames[off>>12])+inside;
        if(to_user)memcpy(u,k,n);else memcpy(k,u,n);
        off+=n;k+=n;length-=n;
    }
    return 0;
}
int ku_copyin(const ku_space *s,void *k,uint32_t off,uint32_t len) {return copy(s,off,k,len,0,0);}
int ku_copyout(const ku_space *s,uint32_t off,const void *k,uint32_t len) {return copy(s,off,(void *)k,len,1,0);}
int ku_load(ku_space *s,uint32_t off,const void *k,uint32_t len) {
    if(!s || s->pins)return -1;
    return copy(s,off,(void *)k,len,1,1);
}
