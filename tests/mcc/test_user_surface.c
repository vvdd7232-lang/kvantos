#define main old_page_model_main
#include "test_user_space.c"
#undef main
#include "user_surface.h"
#include "user_service.h"
static ku_shared shared;
static ku_surface surface;
static ks_service service,foreign;
static int call(ks_service *s,ks_request r) {
    assert(!ku_copyout(s->space,0,&r,sizeof(r)));
    return ks_dispatch(s,0,sizeof(r));
}
static void shared_suite(int arch) {
    uint32_t root=setup(arch);
    assert(!ku_create(&a,&ops,root,arch,arch) && !ku_create(&b,&ops,root,arch,arch));
    unsigned tables=live;
    for(unsigned cut=0;cut<3;cut++) {
        fail_at=calls+cut;
        assert(ku_shared_create(&shared,&ops,8193)<0 && !shared.live && live==tables);
    }
    fail_at=~0u;
    assert(ku_shared_create(&shared,&ops,0)<0 && ku_shared_create(&shared,&ops,~0u)<0);
    assert(!ku_shared_create(&shared,&ops,8193) && shared.pages==3 && shared.refs==1);
    assert(ku_map_shared(&a,0,&shared,KU_R|KU_X)<0);
    assert(ku_map_shared(&a,KU_SIZE-4096,&shared,KU_R)<0);
    assert(ku_map_shared(&a,1,&shared,KU_R)<0);
    assert(!ku_map_shared(&a,4096,&shared,KU_R|KU_W));
    assert(!ku_map_shared(&a,16*4096,&shared,KU_R|KU_W));
    assert(!ku_map_shared(&b,0,&shared,KU_R|KU_W));
    assert(shared.refs==10 && live==tables+3);
    uint32_t color=0xc0aabbcc,got=0;
    assert(!ku_copyout(&a,4096,&color,4) && !ku_copyin(&b,&got,0,4) && got==color);
    assert(ku_check(&a,4096,1,KU_X)<0);
    if(arch)assert(!cpu(&a,KU_BASE+4096,1,0,1));
    assert(!ku_pin(&a));
    assert(ku_unmap_shared(&a,&shared)<0 && ku_protect_shared(&a,&shared,KU_R)<0 && ku_destroy(&a)<0);
    assert(!ku_unpin(&a));
    /* Corrupt a later alias; protection must preflight, not change earlier ones. */
    if(arch) {
        uint64_t *pt=d(NULL,a.tables[3]);uint64_t old=pt[16];pt[16]&=~4ull;
        assert(ku_protect_shared(&a,&shared,KU_R)<0 && (a.rights[1]&KU_W));pt[16]=old;
    } else {
        uint32_t *pt=d(NULL,a.tables[1]);uint32_t old=pt[528];pt[528]&=~4u;
        assert(ku_protect_shared(&a,&shared,KU_R)<0 && (a.rights[1]&KU_W));pt[528]=old;
    }
    assert(!ku_protect_shared(&a,&shared,KU_R));
    assert(!cpu(&a,KU_BASE+4096,1,1,0) && !cpu(&a,KU_BASE+16*4096,1,1,0));
    assert(ku_copyout(&a,4096,&color,4)<0 && ku_copyout(&a,16*4096,&color,4)<0);
    assert(cpu(&b,KU_BASE,1,1,0)); /* protections are per-space, not global */
    assert(!ku_shared_destroy(&shared) && shared.live && !shared.owner && shared.refs==9);
    assert(!ku_shared_destroy(&shared) && shared.refs==9);
    assert(ku_map_shared(&a,32*4096,&shared,KU_R)<0); /* owner revoked */
    assert(!ku_unmap_shared(&a,&shared) && shared.refs==3 && shared.live);
    assert(!ku_copyin(&b,&got,0,4) && got==color); /* no premature zeroing */
    assert(!ku_destroy(&a) && !ku_destroy(&b) && !shared.live && !live);
    printf("PASS %s shared frames: allocator cuts, cross-space visibility, alias protection, NX policy, pinned refusal, revoked owner/mapping drain, zero-on-final-release\n",arch?"x64":"i386");
}
static void surface_suite(int arch) {
    uint32_t root=setup(arch);
    assert(!ku_create(&a,&ops,root,arch,arch) && !ku_map(&a,0,KU_R|KU_W));
    unsigned before=live;
    for(unsigned cut=0;cut<4;cut++) {
        fail_at=calls+cut;
        assert(ku_surface_create(&surface,&a,11,32,40)<0 && !surface.live && live==before);
        for(unsigned j=1;j<KU_PAGES;j++)assert(!a.frames[j] && !a.shared[j]);
    }
    fail_at=~0u;
    assert(ku_surface_create(&surface,&a,0,420,300)<0);
    assert(ku_surface_create(&surface,&a,11,0,300)<0);
    assert(ku_surface_create(&surface,&a,11,4096,4096)<0);
    assert(!ku_surface_create(&surface,&a,11,420,300));
    assert(surface.info.width==420 && surface.info.stride==1680 && surface.buffer[0].pages==124);
    uint32_t got=0;assert(ku_surface_pixel(&surface,0,0,&got)<0);
    uint32_t color=0xff012345;
    for(unsigned i=0;i<100;i++) {
        unsigned back=surface.front^1;
        uint32_t offset=surface.info.offset[back];
        color++;
        assert(!ku_copyout(&a,offset,&color,4) && !ku_copyout(&a,offset+503996,&color,4));
        assert(ku_surface_queue(&surface,surface.front,i+1)<0);
        assert(ku_surface_queue(&surface,back,i+2)<0);
        assert(!ku_surface_queue(&surface,back,i+1) && ku_surface_queue(&surface,back,i+1)<0);
        assert(!ku_pin(&a) && ku_surface_commit(&surface)<0 && ku_surface_destroy(&surface)<0);
        assert(!ku_unpin(&a) && !ku_surface_commit(&surface));
        assert(surface.front==back && surface.sequence==i+1);
        assert(ku_copyout(&a,offset,&color,4)<0 && !cpu(&a,KU_BASE+offset,1,1,0));
        assert(!ku_surface_pixel(&surface,0,0,&got) && got==color);
        assert(!ku_surface_pixel(&surface,419,299,&got) && got==color);
    }
    assert(ku_surface_pixel(&surface,420,0,&got)<0 && ku_surface_pixel(&surface,0,300,&got)<0);
    surface.sequence=0xfffffffeu;
    assert(!ku_surface_queue(&surface,surface.front^1,0xffffffffu) && !ku_surface_commit(&surface));
    assert(ku_surface_queue(&surface,surface.front^1,0)<0);
    assert(!ku_surface_destroy(&surface) && live==before);
    assert(!ku_surface_create(&surface,&a,12,32,32));
    /* Client dies first: kernel ownership keeps frames alive until teardown. */
    assert(!ku_destroy(&a) && surface.buffer[0].refs==1);
    assert(!ku_surface_destroy(&surface) && !live);
    printf("PASS %s double surfaces: all OOM cuts, bounds/overflow, 100 immutable-front flips, page-end pixels, stale sequence, pinned refusal, client-death cleanup\n",arch?"x64":"i386");
}
static void protocol_suite(int arch) {
    uint32_t root=setup(arch);
    assert(!ku_create(&a,&ops,root,arch,arch) && !ku_map(&a,0,KU_R|KU_W));
    assert(!ku_create(&b,&ops,root,arch,arch) && !ku_map(&b,0,KU_R|KU_W));
    assert(!ku_surface_create(&surface,&a,11,32,32));
    assert(!ks_init(&service,&a,11,32,32,NULL,NULL));
    assert(!ks_init(&foreign,&b,22,32,32,NULL,NULL));
    service.protocol=foreign.protocol=2;service.surface=foreign.surface=&surface;
    assert(!ku_pin(&a) && !ku_pin(&b));
    ks_request r={2,sizeof(r),KS_SURFACE_INFO,0,11,0,256,32,0,0};
    assert(call(&service,r)==32);
    ku_surface_info descriptor;
    assert(!ku_copyin(&a,&descriptor,256,32) && descriptor.offset[1]==surface.info.offset[1]);
    r.generation=22;assert(call(&foreign,r)==KS_DENIED);r.generation=11;
    r.version=1;assert(call(&service,r)==KS_INVALID);r.version=2;
    r.op=KS_SURFACE_PRESENT;r.a=1;r.b=1;assert(call(&service,r)==KS_STALE);
    assert(!ks_post(&service,KS_EVENT_DRAW,0,0,0,0,0));
    r.op=KS_WAIT;r.a=256;r.b=48;assert(call(&service,r)==48);
    r.op=KS_SURFACE_PRESENT;r.a=1;r.b=1;r.token=service.event.id;
    uint32_t color=0x80ffaa11;
    assert(!ku_copyout(&a,descriptor.offset[1],&color,4));
    assert(!call(&service,r) && !surface.presented && surface.pending==2);
    assert(call(&service,r)==KS_BUSY);
    r.op=KS_COMPLETE;r.a=r.b=0;assert(!call(&service,r));
    /* Mirror runtime COMPLETE -> kernel CR3 -> unpin -> commit -> repin. */
    assert(!ku_unpin(&a) && !ku_surface_commit(&surface) && !ku_pin(&a));
    assert(ku_copyout(&a,descriptor.offset[1],&color,4)<0);
    uint32_t got;assert(!ku_surface_pixel(&surface,0,0,&got) && got==color);
    ks_destroy(&service);ks_destroy(&foreign);
    assert(!ku_unpin(&a) && !ku_unpin(&b) && !ku_surface_destroy(&surface));
    assert(!ku_destroy(&a) && !ku_destroy(&b) && !live);
    printf("PASS %s protocol2: offset-only descriptor, owner/generation/version, phase/token, deferred present until COMPLETE, front becomes read-only\n",arch?"x64":"i386");
}
int main(void) {for(int arch=0;arch<2;arch++){shared_suite(arch);surface_suite(arch);protocol_suite(arch);}return 0;}
