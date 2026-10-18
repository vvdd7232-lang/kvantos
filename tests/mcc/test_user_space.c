#include "user_space.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define FRAMES 1024
static unsigned char memory[FRAMES][4096] __attribute__((aligned(4096)));
static unsigned char used[FRAMES],saved[64][4096];
static unsigned live,calls,fail_at;
static ku_space a,b;
static void *d(void *ctx,uint32_t p) {(void)ctx;return p && !(p&4095) && p/4096<FRAMES?memory[p/4096]:NULL;}
static uint32_t alloc(void *ctx) {
    (void)ctx;if(calls++==fail_at)return 0;
    for(unsigned i=64;i<FRAMES;i++)if(!used[i]) {used[i]=1;live++;memset(memory[i],0xa5,4096);return i*4096;}
    return 0;
}
static void release(void *ctx,uint32_t p) {
    (void)ctx;unsigned f=p/4096;assert(f>=64 && f<FRAMES && used[f]);
    for(unsigned i=0;i<4096;i++)assert(!memory[f][i]);
    used[f]=0;live--;
}
static ku_ops ops={alloc,release,d,NULL};
static uint32_t setup(int arch) {
    assert(!live);memset(memory,0,sizeof(memory));memset(used,0,sizeof(used));
    memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));calls=0;fail_at=~0u;
    if(arch) {
        ((uint64_t *)memory[1])[0]=0x2003;
        ((uint64_t *)memory[2])[0]=0x3003;
        uint64_t *pd=(uint64_t *)memory[3];
        pd[0]=0x4003;
        for(unsigned i=1;i<64;i++)pd[i]=((uint64_t)i<<21)|131;
        for(unsigned i=1;i<512;i++)((uint64_t *)memory[4])[i]=i*4096+3;
    } else {
        for(unsigned i=0;i<32;i++) {
            ((uint32_t *)memory[1])[i]=(2+i)*4096+3;
            for(unsigned j=0;j<1024;j++)((uint32_t *)memory[2+i])[j]=(i*1024+j)*4096+3;
        }
        ((uint32_t *)memory[2])[0]=0;
    }
    memcpy(saved,memory,sizeof(saved));return 4096;
}
/* Independent CPU-shaped walker; can inspect inherited supervisor mappings. */
static uint32_t cpu(const ku_space *s,uint32_t va,int user,int write,int exec) {
    uint32_t p=s->root;
    for(int l=s->arch64?3:1;l>=0;l--) {
        unsigned i=((uint64_t)va>>(12+(s->arch64?9:10)*l))&(s->arch64?511:1023);
        uint64_t e=s->arch64?((uint64_t *)d(NULL,p))[i]:((uint32_t *)d(NULL,p))[i];
        if(!(e&1) || (user && !(e&4)) || (write && !(e&2)) || (exec && s->nx && (e>>63)))return 0;
        if(l && (e&128))return (uint32_t)(e&0xffe00000)+(va&0x1fffff);
        p=(uint32_t)(e&0xfffff000);if(!d(NULL,p))return 0;
    }
    return p+(va&4095);
}
static unsigned char payload[6000],out[6000];
static void suite(int arch) {
    unsigned tables=arch?4:2;uint32_t root;
    for(unsigned fail=0;fail<tables;fail++) {
        root=setup(arch);fail_at=fail;
        assert(ku_create(&a,&ops,root,arch,arch)<0 && !a.live && !live);
        assert(!memcmp(saved,memory,sizeof(saved)));
    }
    root=setup(arch);
    assert(!ku_create(&a,&ops,root,arch,arch) && live==tables);
    assert(!ku_create(&b,&ops,root,arch,arch) && live==2*tables);
    assert(!memcmp(saved,memory,sizeof(saved)));
    assert(!cpu(&a,0x100000,1,0,0) && cpu(&a,0x100000,0,0,0)==0x100000);
    assert(!cpu(&a,KU_BASE,1,0,0));
    assert(ku_map(&a,0,KU_R|KU_W|KU_X)<0 && ku_map(&a,0,KU_W)<0);
    assert(ku_map(&a,1,KU_R)<0 && ku_map(&a,KU_SIZE,KU_R)<0);
    assert(!ku_map(&a,0,KU_R|KU_X) && !ku_map(&a,4096,KU_R|KU_W));
    assert(!ku_map(&a,KU_SIZE-4096,KU_R|KU_W));
    assert(!ku_map(&b,0,KU_R|KU_W) && b.frames[0]!=a.frames[0]);
    assert(ku_map(&a,0,KU_R)<0);
    assert(cpu(&a,KU_BASE,1,0,1)==a.frames[0]);
    assert(!cpu(&a,KU_BASE,1,1,0));
    if(arch)assert(!cpu(&a,KU_BASE+4096,1,0,1));
    assert(!ku_check(&a,0,4096,KU_R|KU_X));
    assert(ku_check(&a,4096,1,KU_X)<0);
    for(unsigned i=0;i<6000;i++)payload[i]=(unsigned char)(i*37+11);
    assert(!ku_load(&a,0,payload,6000));
    assert(!ku_copyin(&a,out,0,6000) && !memcmp(out,payload,6000));
    assert(ku_copyout(&a,0,payload,6000)<0); /* no partial write into RX */
    assert(!ku_copyout(&b,0,payload,4096));
    memset(out,0x5a,sizeof(out));
    assert(ku_copyin(&a,out,4096,6000)<0); /* last page absent: output untouched */
    for(unsigned i=0;i<6000;i++)assert(out[i]==0x5a);
    assert(ku_copyout(&a,4096,payload,6000)<0);
    assert(!memcmp(d(NULL,a.frames[1]),payload+4096,1904));
    assert(ku_check(&a,0xffffffff,1,KU_R)<0 && ku_check(&a,KU_SIZE,1,KU_R)<0);
    assert(!ku_copyin(&a,NULL,KU_SIZE,0) && ku_copyin(&a,NULL,KU_SIZE+1,0)<0);
    assert(ku_copyin(&a,NULL,0,1)<0);
    assert(!ku_pin(&a) && ku_destroy(&a)<0 && ku_map(&a,8192,KU_R)<0 && ku_load(&a,0,payload,1)<0);
    assert(!ku_unpin(&a) && ku_unpin(&a)<0);
    /* Change actual U/S, RW and physical PTE: metadata alone must not grant access. */
    if(arch) {
        uint64_t *pt=d(NULL,a.tables[3]),e=pt[1];
        pt[1]&=~4ull;assert(ku_check(&a,4096,1,KU_R)<0);pt[1]=e;
        pt[1]&=~2ull;assert(ku_check(&a,4096,1,KU_W)<0);pt[1]=e;
        pt[1]=b.frames[0]|7;assert(ku_check(&a,4096,1,KU_R)<0);pt[1]=e;
        uint64_t *r=d(NULL,a.root),old=r[0];r[0]&=~4ull;
        assert(ku_check(&a,0,1,KU_R)<0);r[0]=old;
    } else {
        uint32_t *pt=d(NULL,a.tables[1]),e=pt[513];
        pt[513]&=~4u;assert(ku_check(&a,4096,1,KU_R)<0);pt[513]=e;
        pt[513]&=~2u;assert(ku_check(&a,4096,1,KU_W)<0);pt[513]=e;
        pt[513]=b.frames[0]|7;assert(ku_check(&a,4096,1,KU_R)<0);pt[513]=e;
        uint32_t *r=d(NULL,a.root),old=r[3];r[3]&=~4u;
        assert(ku_check(&a,0,1,KU_R)<0);r[3]=old;
    }
    assert(!ku_destroy(&a) && !ku_destroy(&a) && !ku_destroy(&b) && !live);
    assert(!memcmp(saved,memory,sizeof(saved)));
    root=setup(arch);assert(!ku_create(&a,&ops,root,arch,arch));
    for(unsigned i=0;i<KU_PAGES;i++)assert(!ku_map(&a,i*4096,KU_R|KU_W));
    assert(!ku_check(&a,0,KU_SIZE,KU_R|KU_W));
    assert(!ku_destroy(&a) && !live);
    for(unsigned run=0;run<100;run++) {
        assert(!ku_create(&a,&ops,root,arch,arch));
        fail_at=calls;assert(ku_map(&a,0,KU_R)<0 && !a.frames[0]);fail_at=~0u;
        assert(!ku_map(&a,0,KU_R|KU_W));assert(!ku_destroy(&a) && !live);
    }
    printf("PASS %s private spine/page walks, W^X/NX policy, copies/overflow/guards, isolation, OOM rollback, zeroing/pins/100 teardown cycles\n",arch?"x64":"i386");
}
static unsigned char private_tables[4][4096],private_user[4096];
static void coherence(int arch) {
    uint32_t root=setup(arch);assert(!ku_create(&a,&ops,root,arch,arch));
    assert(!ku_map(&a,0,KU_R|KU_W) && !ku_copyout(&a,0,payload,4096) && !ku_pin(&a));
    uint32_t fresh_va=arch?0xc0100000u:(40u<<22)|(17u<<12);
    assert(!cpu(&a,fresh_va,0,0,0));
    if(arch) {
        ((uint64_t *)memory[2])[3]=0x5007;
        ((uint64_t *)memory[5])[0]=0x200087;
        ((uint64_t *)memory[3])[100]=0x200087;
        ((uint64_t *)memory[1])[1]=0x6007;
    } else {
        ((uint32_t *)memory[1])[40]=34*4096+7;
        ((uint32_t *)memory[34])[17]=0x300007;
        ((uint32_t *)memory[5])[1]=0x210007;
    }
    unsigned allocations=calls,owned=live;
    memcpy(private_user,d(NULL,a.frames[0]),4096);
    assert(!ku_refresh(&a,root) && calls==allocations && live==owned && a.pins==1);
    assert(cpu(&a,fresh_va,0,0,0)==0x300000 && !cpu(&a,fresh_va,1,0,0));
    if(arch) {
        assert(cpu(&a,(100u<<21)|4096,0,0,0)==0x201000);
        assert(!cpu(&a,(100u<<21)|4096,1,0,0));
        assert(((uint64_t *)d(NULL,a.root))[1]==0x6003);
    } else {
        assert(cpu(&a,(3u<<22)|4096,0,0,0)==0x210000);
        assert(!cpu(&a,(3u<<22)|4096,1,0,0));
    }
    assert(!ku_check(&a,0,4096,KU_R|KU_W));
    assert(!memcmp(private_user,d(NULL,a.frames[0]),4096));
    for(unsigned i=0;i<a.table_count;i++)memcpy(private_tables[i],d(NULL,a.tables[i]),4096);
    /* Replacing the borrowed spine is rejected before ANY private-table write. */
    if(arch)((uint64_t *)memory[1])[0]=0x6003;
    else ((uint32_t *)memory[1])[3]=0x6003;
    assert(ku_refresh(&a,root)<0 && ku_refresh(&a,root+4096)<0);
    for(unsigned i=0;i<a.table_count;i++)assert(!memcmp(private_tables[i],d(NULL,a.tables[i]),4096));
    if(arch)((uint64_t *)memory[1])[0]=0x2003;
    else ((uint32_t *)memory[1])[3]=0x5003;
    assert(!ku_refresh(&a,root));
    assert(!ku_unpin(&a) && !ku_destroy(&a) && !live && ku_refresh(&a,root)<0);
    printf("PASS %s pinned supervisor-map refresh, new MMIO ancestors, user isolation, stable user data, replaced-spine atomic refusal\n",arch?"x64":"i386");
}
int main(void) {suite(0);suite(1);coherence(0);coherence(1);return 0;}
