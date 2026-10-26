#define main page_model_main_unused
#include "test_user_space.c"
#undef main
#include "user_image.h"
#include <stdlib.h>
static uint8_t image[KI_HEADER+KI_PAYLOAD_MAX];
static void put(uint8_t *b,unsigned off,uint32_t n) {for(unsigned i=0;i<4;i++)b[off+i]=(uint8_t)(n>>(8*i));}
static void fixture(int arch) {
    memset(image,0,sizeof(image));memcpy(image,"KAPP",4);image[4]=2;image[6]=96;
    put(image,8,4);put(image,12,1|(arch?256:0));put(image,16,KU_BASE);put(image,20,KU_BASE);put(image,24,20);put(image,28,12);
    memcpy(image+32,"Private",8);put(image,64,16);put(image,68,4096);put(image,72,16);put(image,76,4);put(image,80,12);put(image,84,8192);put(image,88,1);
    memset(image+96,0x90,16);memcpy(image+112,"data",4);
}
int main(int argc,char **argv) {
    for(int arch=0;arch<=1;arch++) {
        unsigned allocations=(arch?4:2)+5;
        for(unsigned fail=0;fail<allocations;fail++) {
            uint32_t root=setup(arch);fixture(arch);ki_plan plan;memset(&plan,0xa5,sizeof(plan));ki_plan old=plan;fail_at=fail;
            assert(ki_load(&a,&ops,root,image,116,arch,arch,&plan)<0 && !a.live && !live && !memcmp(&plan,&old,sizeof(plan)));
        }
        uint32_t root=setup(arch);fixture(arch);ki_plan plan;
        assert(!ki_validate(image,116,arch,&plan) && ki_validate(image,116,!arch,&plan)<0);
        assert(!ki_load(&a,&ops,root,image,116,arch,arch,&plan));
        uint8_t data[16];assert(!ku_copyin(&a,data,4096,16) && !memcmp(data,"data",4));
        for(unsigned i=4;i<16;i++)assert(!data[i]);
        assert(ku_copyout(&a,0,data,1)<0 && ku_copyout(&a,KI_BOOT_OFF,data,1)<0);
        assert(ku_check(&a,KU_SIZE-8192-1,1,KU_R)<0 && !ku_check(&a,KU_SIZE-8192,8192,KU_R|KU_W));
        assert(!ku_destroy(&a) && !live);
        const unsigned bad[]={8,12,16,20,24,28,64,68,72,76,80,84,88,92};
        for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
            fixture(arch);put(image,bad[i],0xffffffff);assert(ki_validate(image,116,arch,&plan)<0);
        }
        fixture(arch);put(image,88,2);assert(!ki_validate(image,116,arch,&plan) && plan.protocol==2);
        fixture(arch);memset(image+32,'x',32);assert(ki_validate(image,116,arch,&plan)<0);
        fixture(arch);assert(ki_validate(image,115,arch,&plan)<0 && ki_validate(image,117,arch,&plan)<0);
        uint32_t seed=4242;
        for(unsigned i=0;i<20000;i++) {
            fixture(arch);seed=seed*1664525u+1013904223u;image[seed%116]^=(uint8_t)(1+(seed>>16)%255);
            (void)ki_validate(image,116,arch,&plan);
        }
        printf("PASS %s image layout/overflow/arch/entry/W^X/guards/BSS, OOM cuts + 20000 mutations\n",arch?"x64":"i386");
    }
    if(argc==3) {
        FILE *f=fopen(argv[1],"rb");assert(f);size_t n=fread(image,1,sizeof(image),f);assert(feof(f));fclose(f);
        ki_plan p;assert(!ki_validate(image,(uint32_t)n,atoi(argv[2]),&p));printf("PASS SDK image %s\n",argv[1]);
    }
    return 0;
}
