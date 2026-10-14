/* Deterministic mutation tests of the compiler, never executing user input. */
#include "mcc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned rng = 4242;
static unsigned random_word(void) { rng = rng * 1664525u + 1013904223u; return rng; }
static char source[MCC_SOURCE_MAX + 2];
static unsigned char out[MCC_OUTPUT_MAX];
int main(void) {
    const char *samples[] = {
        "void draw(void){clear(rgb(1,2,3));}",
        "int x=1;int f(int a){return a+x;}void draw(void){clear(f(4));}",
        "void draw(void){int i=0;while(i<5){i++;if(i==3)continue;clear(i);}}",
        "char *s=\"hello\";void draw(void){text(1,2,s,-1,-1);}",
        "void draw(void){int x=1;int *p=&x;for(int i=0;i<8;i++){(*p)++;}clear(x);}"
    };
    for (int iteration = 0; iteration < 40000; iteration++) {
        const char *sample = samples[random_word() % 5];
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
        int rc = mcc_compile(source, "Fuzz", out, sizeof(out), &size);
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
    assert(mcc_compile("void draw(void){}", "x", out, 64, &size) < 0 && size == 0);
    memset(source, ' ', MCC_SOURCE_MAX + 1); source[MCC_SOURCE_MAX + 1] = 0;
    assert(mcc_compile(source,"x",out,sizeof(out),&size)<0);
    assert(mcc_compile("void draw(void){}","x",out,sizeof(out),&size)==0);
    puts("40000 compiler mutations clean under ASan/UBSan");
    return 0;
}
