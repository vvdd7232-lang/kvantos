/* Exact compiler resource boundaries, both targets, with inaccessible pages.
   Includes production MCC to derive actual table use, not stale magic numbers. */
#include "../../kernel/mcc.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
static u8 output[MCC_ISOLATED_OUTPUT_MAX];
static char source[MCC_SOURCE_MAX+2];
static unsigned checks;
static void check_source(const char *text,int mode,const char *error) {
    memset(output,0xa5,sizeof(output));u32 bytes=123;
    int rc=mcc_compile_target(text,"Test",output,sizeof(output),&bytes,mode);
    if(error) {
        if(rc!=-1 || bytes || !strstr(mcc_error(),error))fprintf(stderr,"case%u mode%d expected %s; rc%d bytes%u actual %s\n",checks,mode,error,rc,bytes,mcc_error());
        assert(rc==-1 && !bytes && strstr(mcc_error(),error));
        for(unsigned i=0;i<sizeof(output);i++)assert(output[i]==0xa5);
    } else assert(!rc && bytes>64 && bytes<=sizeof(output));
    checks++;
}
static void table_boundaries(int mode) {
    check_source("void draw(void){}",mode,NULL);int builtin_count=ns-1;
    const char *header="#include <kvui.h>\n";int n=0;
    for(int extra=0;extra<2;extra++) {
        n=snprintf(source,sizeof(source),"%s",header);
        for(int i=0;i<PP_MACROS-13+extra;i++)n+=snprintf(source+n,sizeof(source)-n,"#define M%d %d\n",i,i);
        strcpy(source+n,"void draw(void){}");check_source(source,mode,extra?"macro count limit":NULL);
        n=snprintf(source,sizeof(source),"%s",header);
        for(int i=0;i<ALIAS_MAX-3+extra;i++)n+=snprintf(source+n,sizeof(source)-n,"typedef int T%d;",i);
        strcpy(source+n,"void draw(void){}");check_source(source,mode,extra?"typedef":NULL);
        n=snprintf(source,sizeof(source),"%s",header);
        for(int i=0;i<RECORD_MAX-1+extra;i++)n+=snprintf(source+n,sizeof(source)-n,"struct S%d;",i);
        strcpy(source+n,"void draw(void){}");check_source(source,mode,extra?"struct":NULL);
        n=snprintf(source,sizeof(source),"%sstruct S{",header);
        for(int i=0;i<MEMBER_MAX-2+extra;i++)n+=snprintf(source+n,sizeof(source)-n,"int m%d;",i);
        strcpy(source+n,"};void draw(void){}");check_source(source,mode,extra?"member":NULL);
        n=0;for(int i=0;i<SYM_MAX-builtin_count-1+extra;i++)n+=snprintf(source+n,sizeof(source)-n,"int s%d;",i);
        strcpy(source+n,"void draw(void){}");check_source(source,mode,extra?"too many symbols":NULL);
        n=snprintf(source,sizeof(source),"void draw(void){");
        for(int i=0;i<LOCAL_MAX+extra;i++)n+=snprintf(source+n,sizeof(source)-n,"int l%d;",i);
        strcpy(source+n,"}");check_source(source,mode,extra?"local":NULL);
        int base=align_to((MCC_TARGET64?72:40)+5,MCC_WORD);
        snprintf(source,sizeof(source),"char payload[%d];void draw(void){}",DATA_MAX-base+extra);
        check_source(source,mode,extra?"data limit":NULL);
    }
    /* No hidden 128-row limit; byte budget is independent of row count. */
    const char *tail="void draw(void){}";size_t length=strlen(tail);
    memset(source,'\n',MCC_SOURCE_MAX-length);strcpy(source+MCC_SOURCE_MAX-length,tail);
    check_source(source,mode,NULL);
    /* Use a shorter prefix to keep this failing case within source16000. */
    memset(source,'\n',15000);strcpy(source+15000,"void draw(void){missing();}");
    check_source(source,mode,"unknown");assert(mcc_error_line()==15001);
    /* UTF-8 comment bytes count toward the same budget, not code points. */
    int newline=MCC_SOURCE_MAX-(int)length-1;source[0]=source[1]='/';
    for(int i=2;i<newline;i++)source[i]=(char)(i&1?0xaf:0xd0);
    source[newline]='\n';strcpy(source+newline+1,tail);check_source(source,mode,NULL);
    for(int extra=0;extra<2;extra++) {
        n=snprintf(source,sizeof(source),"void draw(void){while(1){");
        for(int i=0;i<256+extra;i++)n+=snprintf(source+n,sizeof(source)-n,"break;");
        strcpy(source+n,"}}");check_source(source,mode,extra?"break":NULL);
    }
    /* Internal emit/table guards are reachable before touching a full array. */
    error_text=NULL;nc=CODE_MAX;byte(0x90);assert(error_text && nc==CODE_MAX);checks++;
    error_text=NULL;nd=DATA_MAX;reserve_data(1);assert(error_text && nd==DATA_MAX);checks++;
    error_text=NULL;nf=FIX_MAX;fix(0,0,0,0,0);assert(error_text && nf==FIX_MAX);checks++;
    error_text=NULL;tokens=24000;input="";next();assert(error_text && !token);checks++;
    error_text=NULL;depth=24;assert(!enter() && depth==24 && error_text);checks++;
    check_source("void draw(void){}",mode,NULL);
}
int main(void) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE),usable=(MCC_SOURCE_MAX+2+page-1)/page*page;
    char *map=mmap(NULL,usable+page*2,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(map!=MAP_FAILED && !mprotect(map+page,usable,PROT_READ|PROT_WRITE));
    char *end=map+page+usable;
    for(int mode=0;mode<2;mode++) {
        for(unsigned length=MCC_SOURCE_MAX-1;length<=MCC_SOURCE_MAX+1;length++) {
            char *text=end-length-1;memset(text,'x',length);text[0]='/';text[1]='*';
            const char *tail="*/void draw(void){}";strcpy(text+length-strlen(tail),tail);
            check_source(text,mode,length>MCC_SOURCE_MAX?"source limit":NULL);
        }
        const char *short_bad[]={"#","#i","#inc","#includ","#include","#include <","#include \"","/","/*","\"","'"};
        for(unsigned i=0;i<sizeof(short_bad)/sizeof(short_bad[0]);i++) {
            char *text=end-strlen(short_bad[i])-1;strcpy(text,short_bad[i]);check_source(text,mode,"");
        }
        table_boundaries(mode);
    }
    assert(!munmap(map,usable+page*2));
    printf("PASS %s %u byte/table/row/guard-page budget and transactional recovery checks\n",MCC_TARGET64?"x64":"i386",checks);
    return 0;
}
