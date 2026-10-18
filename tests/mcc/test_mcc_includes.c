/* Guard-only registry enabled solely in this standalone host test binary. */
#include "../../kernel/mcc.c"
#include <assert.h>
#include <stdio.h>
static u8 output[MCC_ISOLATED_OUTPUT_MAX];
static char oversized[PP_OUTPUT+2];
static void bad(const char *source,const char *message,int row) {
    memset(output,0xa5,sizeof(output));u32 bytes=123;
    assert(mcc_compile_target(source,"Test",output,sizeof(output),&bytes,1)<0);
    assert(!bytes && strstr(mcc_error(),message) && mcc_error_line()==row);
    for(unsigned i=0;i<sizeof(output);i++)assert(output[i]==0xa5);
}
int main(void) {
    u32 bytes;
    const char *source="#include <kvui.h>\n#include \"kvtypes.h\"\nvoid draw(void){kv_point p;p.x=KV_CHAR_W;clear(p.x);}";
    assert(!mcc_compile_target(source,"Test",output,sizeof(output),&bytes,1));
    assert(pp_count==13 && pp_included==7 && !pp_including);
    bad("\n#include \"cycle1.h\"\nvoid draw(void){}","include cycle",2);
    bad("#include \"depth1.h\"\nvoid draw(void){}","include depth limit",1);
    assert(!mcc_compile_target("#include \"depth2.h\"\nvoid draw(void){deep_int n=7;clear(n);}","Test",output,sizeof(output),&bytes,1));
    bad("#include \"../kvui.h\"\nvoid draw(void){}","paths/escapes",1);
    bad("#include \"/kvui.h\"\nvoid draw(void){}","paths/escapes",1);
    bad("#include <stdio.h>\nvoid draw(void){}","unknown builtin",1);
    bad("#define H \"kvui.h\"\n#include H\nvoid draw(void){}","literal builtin",2);
    bad("#include \"kvui.h\" trailing\nvoid draw(void){}","trailing tokens",1);
    bad("#include \"kvui.h\nvoid draw(void){}","unterminated include",1);
    bad("#include \"kvui.h\" /* unclosed\nvoid draw(void){}","unterminated include comment",1);
    bad("#include \"kvui.h\"\n\nvoid draw(void){missing();}","unknown",3);
    bad("#include <kvui.h>\n#define KV_BG 1\nvoid draw(void){}","redefinition",2);
    error_text=NULL;error_line=token_line=pp_line=1;pp_used=pp_count=0;pp_including=pp_included=0;pp_work=PP_WORK;
    pp_directive("include \"kvui.h\"",0);assert(error_text && strstr(error_text,"include work limit"));
    error_text=NULL;pp_used=pp_work=pp_count=0;pp_line=1;memset(oversized,' ',PP_OUTPUT+1);oversized[PP_OUTPUT+1]=0;
    pp_source(oversized,1);assert(error_text && strstr(error_text,"output limit") && pp_used==PP_OUTPUT);
    assert(!mcc_compile_target(source,"Test",output,sizeof(output),&bytes,1));
    assert(pp_count==13 && pp_included==7 && !pp_including);
    printf("PASS %s builtin include whitelist/once/depth/cycle/work/output/source-row/transactional reset guards\n",MCC_TARGET64?"x64":"i386");
    return 0;
}
