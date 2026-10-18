/* Deterministic mutation tests of the compiler, never executing user input. */
#include "mcc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned rng = 4242;
static unsigned random_word(void) { rng = rng * 1664525u + 1013904223u; return rng; }
static char source[MCC_SOURCE_MAX + 2];
#ifndef MCC_FUZZ_MODE
#define MCC_FUZZ_MODE 0
#endif
static unsigned char out[MCC_FUZZ_MODE?MCC_ISOLATED_OUTPUT_MAX:MCC_OUTPUT_MAX];
static int compile_fuzz(const char *s,const char *title,u8 *output,u32 capacity,u32 *size) {
    return mcc_compile_target(s,title,output,capacity,size,MCC_FUZZ_MODE);
}
int main(void) {
    const char *samples[] = {
        "void draw(void){clear(rgb(1,2,3));}",
        "void draw(void){int h;int m;int s;clock(&h,&m,&s);clock(0,0,0);}",
        "#include <kvui.h>\nvoid draw(void){kv_point p;p.x=KV_CHAR_W;clear(p.x);}",
        "#include \"kvui.h\"\n#include <kvui.h>\nvoid draw(void){clear(KV_BG);}",
        "#include \"../kvui.h\"\nvoid draw(void){}",
        "#include \"kvui.h\" trailing\nvoid draw(void){}",
        "#include <kvtypes.h>\nvoid draw(void){kv_byte b=7;clear(b);}",
        "int x=1;int f(int a){return a+x;}void draw(void){clear(f(4));}",
        "void draw(void){int i=0;while(i<5){i++;if(i==3)continue;clear(i);}}",
        "char *s=\"hello\";void draw(void){text(1,2,s,-1,-1);}",
        "void draw(void){int x=1;int *p=&x;for(int i=0;i<8;i++){(*p)++;}clear(x);}",
        "int a[5];void draw(void){char b[7];int *p[2];p[0]=a;b[0]=1;clear(sizeof b+sizeof(p)+sizeof(a[0]++));}",
        "void draw(void){int a[2];a[0]=7;clear(sizeof(a=0));}",
        "#define N 3\n#define X (N+1)\nvoid draw(void){int a[N];a[0]=X;clear(a[0]);}",
        "#define A B\n#define B A\nvoid draw(void){clear(A);}",
        "typedef struct N{int n;struct N *next;} Node;void draw(void){Node a,b;a.next=&b;b.n=3;clear(a.next->n);}",
        "struct S{char a[5];int n;};void draw(void){struct S a[3];a[2].n=9;clear(a[2].n);}",
        "typedef int Number;typedef Number *P;void draw(void){Number n=1;P p=&n;clear(*p);}"
    };
    for (int iteration = 0; iteration < 40000; iteration++) {
        const char *sample = samples[random_word() % (sizeof(samples)/sizeof(samples[0]))];
        unsigned len = (unsigned)strlen(sample);
        memcpy(source, sample, len + 1);
        unsigned mutations = 1 + random_word() % 12;
        for (unsigned i = 0; i < mutations; i++) {
            unsigned at = random_word() % (len + 1);
            if (random_word() & 1) {
                if (at < len) source[at] = (char)(1 + random_word() % 127);
            } else if (len + 1 < sizeof(source)) {
                memmove(source + at + 1, source + at, len - at + 1);
                source[at] = (char)(1 + random_word() % 127);
                len++;
            }
        }
        memset(out, 0xA5, sizeof(out));
        u32 size = 123;
        int rc = compile_fuzz(source, "Fuzz", out, sizeof(out), &size);
        if (rc) {
            assert(size == 0 && mcc_error()[0]);
            for (unsigned i = 0; i < sizeof(out); i++) assert(out[i] == 0xA5);
        } else {
            assert(size > 64 && size <= sizeof(out));
            assert(!memcmp(out, "KAPP", 4));
        }
    }
    /* Output capacity, token/code/data limits and failure recovery. */
    u32 size;
    assert(compile_fuzz("void draw(void){}", "x", out, 64, &size) < 0 && size == 0);
    memset(source, ' ', MCC_SOURCE_MAX + 1); source[MCC_SOURCE_MAX + 1] = 0;
    assert(compile_fuzz(source,"x",out,sizeof(out),&size)<0);
    assert(compile_fuzz("void draw(void){}","x",out,sizeof(out),&size)==0);
    printf("40000 compiler mutations clean under ASan/UBSan, target mode %d\n",MCC_FUZZ_MODE);
    return 0;
}
