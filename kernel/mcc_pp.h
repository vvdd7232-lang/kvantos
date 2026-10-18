/* Bounded object-like preprocessing for the Workshop, not a general CPP.
   Included by mcc.c: workspace stays in BSS, original newlines are preserved. */
#define PP_MACROS 32
#define PP_BODY 256
#define PP_OUTPUT 32000
#define PP_DEPTH 8
#define PP_WORK 64000
static struct { char name[NAME_MAX], body[PP_BODY]; int active; } pp_macros[PP_MACROS];
static char pp_output[PP_OUTPUT+1];
static int pp_count, pp_used, pp_work, pp_line;
#include "mcc_headers.h"
static void pp_fail(const char *s) { token_line=pp_line; fail(s); }
static int pp_space(int c) { return c==' ' || c=='\t' || c=='\r'; }
static void pp_emit(int c) {
    if(pp_used==PP_OUTPUT) { pp_fail("preprocessor output limit (32000 bytes)"); return; }
    pp_output[pp_used++]=(char)c;
    if(c=='\n') pp_line++;
}
static int pp_find(const char *p,int len) {
    for(int i=0;i<pp_count;i++)
        if((int)strlen(pp_macros[i].name)==len && !memcmp(p,pp_macros[i].name,(size_t)len))return i;
    return -1;
}
static void pp_expand(const char *p,const char *end,int depth) {
    while(p<end && !error_text) {
        if(++pp_work>PP_WORK) { pp_fail("preprocessor work limit (64000 steps)"); return; }
        if(letter((u8)*p)) {
            const char *start=p++;
            while(p<end && (letter((u8)*p) || digit((u8)*p)))p++;
            int m=pp_find(start,(int)(p-start));
            if(m<0) { while(start<p)pp_emit((u8)*start++); }
            else {
                if(pp_macros[m].active) { pp_fail("recursive macro expansion"); return; }
                if(depth>=PP_DEPTH) { pp_fail("macro expansion depth limit (8)"); return; }
                pp_macros[m].active=1;pp_emit(' ');
                pp_expand(pp_macros[m].body,pp_macros[m].body+strlen(pp_macros[m].body),depth+1);
                pp_emit(' ');pp_macros[m].active=0;
            }
        } else if(digit((u8)*p) || (*p=='.' && p+1<end && digit((u8)p[1]))) {
            /* Never expand identifier substrings inside preprocessing numbers. */
            int prev=0;
            do {
                prev=(u8)*p;pp_emit((u8)*p++);
            } while(p<end && (letter((u8)*p) || digit((u8)*p) || *p=='.' ||
                     ((*p=='+' || *p=='-') && (prev=='e' || prev=='E' || prev=='p' || prev=='P'))));
        } else if(*p=='"' || *p=='\'') {
            int quote=*p;pp_emit((u8)*p++);int closed=0;
            while(p<end && *p!='\n' && !error_text) {
                int c=(u8)*p++;pp_emit(c);
                if(c==quote) {closed=1;break;}
                if(c=='\\') {
                    if(p==end || *p=='\n')break;
                    pp_emit((u8)*p++);
                }
            }
            if(!closed)pp_fail("unterminated preprocessor literal");
        } else if(*p=='#') {pp_fail("# and ## in macro replacements are unsupported");return;}
        else pp_emit((u8)*p++);
    }
}
static const char *pp_define(const char *p) {
    const char *end=p;while(*end && *end!='\n')end++;
    while(p<end && pp_space((u8)*p))p++;
    const char *directive=p;while(p<end && letter((u8)*p))p++;
    if(p-directive!=6 || memcmp(directive,"define",6) || p==end || !pp_space((u8)*p)) {
        pp_fail("only object-like #define or builtin #include directives are supported");return end;
    }
    while(p<end && pp_space((u8)*p))p++;
    const char *name=p;
    if(p==end || !letter((u8)*p)) {pp_fail("expected macro name");return end;}
    while(p<end && (letter((u8)*p) || digit((u8)*p)))p++;
    int len=(int)(p-name);
    if(len>=NAME_MAX) {pp_fail("macro name too long");return end;}
    if(p<end && *p=='(') {pp_fail("function-like macros are unsupported");return end;}
    if(p<end && !pp_space((u8)*p) && !(p+1<end && *p=='/' && (p[1]=='/' || p[1]=='*'))) {
        pp_fail("expected whitespace after macro name");return end;
    }
    if(pp_find(name,len)>=0) {pp_fail("macro redefinition is unsupported");return end;}
    if(pp_count==PP_MACROS) {pp_fail("macro count limit (32)");return end;}
    int m=pp_count, used=0;
    memcpy(pp_macros[m].name,name,(size_t)len);pp_macros[m].name[len]=0;
    while(p<end && pp_space((u8)*p))p++;
    /* Definition comments become whitespace; multiline directives are deliberately rejected. */
    while(p<end && !error_text) {
        int c=(u8)*p++;
        if(c=='/' && p<end && *p=='/')break;
        if(c=='/' && p<end && *p=='*') {
            p++;while(p+1<end && !(p[0]=='*' && p[1]=='/'))p++;
            if(p+1>=end) {pp_fail("multiline comment in #define is unsupported");break;}
            p+=2;c=' ';
        } else if(c=='"' || c=='\'') {
            int quote=c,closed=0;
            if(used>=PP_BODY-1) {pp_fail("macro body limit (255 bytes)");break;}
            pp_macros[m].body[used++]=(char)c;
            while(p<end && !error_text) {
                c=(u8)*p++;
                if(used>=PP_BODY-1) {pp_fail("macro body limit (255 bytes)");break;}
                pp_macros[m].body[used++]=(char)c;
                if(c==quote) {closed=1;break;}
                if(c=='\\' && p<end) {
                    if(used>=PP_BODY-1) {pp_fail("macro body limit (255 bytes)");break;}
                    pp_macros[m].body[used++]=*p++;
                }
            }
            if(!closed)pp_fail("unterminated macro literal");
            continue;
        } else if(c=='#' || c=='\\') {
            pp_fail("macro #/## and line continuation are unsupported");break;
        }
        if(used>=PP_BODY-1) {pp_fail("macro body limit (255 bytes)");break;}
        pp_macros[m].body[used++]=(char)c;
    }
    while(used && pp_space((u8)pp_macros[m].body[used-1]))used--;
    pp_macros[m].body[used]=0;pp_macros[m].active=0;pp_count++;
    return end;
}
static void pp_source(const char *,int);
static const char *pp_directive(const char *p,int depth) {
    const char *original=p;while(pp_space((u8)*p))p++;
    if(strncmp(p,"include",7) || (p[7] && !pp_space((u8)p[7]) && p[7]!='"' && p[7]!='<'))return pp_define(original);
    p+=7;while(pp_space((u8)*p))p++;
    const char *end=p;while(*end && *end!='\n')end++;
    if(p==end || (*p!='"' && *p!='<')) {pp_fail("include requires literal builtin header name");return end;}
    int close=*p++=='"'?'"':'>';const char *name=p;
    while(p<end && *p!=close)p++;
    if(p==end) {pp_fail("unterminated include name");return end;}
    unsigned length=(unsigned)(p-name);p++;
    if(!length || length>31) {pp_fail("include name length (1..31)");return end;}
    for(unsigned i=0;i<length;i++)if(!letter((u8)name[i]) && !digit((u8)name[i]) && name[i]!='.') {
        pp_fail("include paths/escapes are forbidden");return end;
    }
    while(p<end) {
        if(pp_space((u8)*p)) {p++;continue;}
        if(p+1<end && p[0]=='/' && p[1]=='/')break;
        if(p+1<end && p[0]=='/' && p[1]=='*') {
            p+=2;while(p+1<end && !(p[0]=='*' && p[1]=='/'))p++;
            if(p+1>=end) {pp_fail("unterminated include comment");return end;}p+=2;continue;
        }
        pp_fail("trailing tokens after include");return end;
    }
    unsigned index=0;while(index<PP_HEADERS && (strlen(pp_headers[index].name)!=length || memcmp(name,pp_headers[index].name,length)))index++;
    if(index==PP_HEADERS) {pp_fail("unknown builtin header (kvui.h/kvkeys.h/kvtypes.h)");return end;}
    unsigned bit=1u<<index;
    if(pp_including&bit) {pp_fail("include cycle");return end;}
    if(pp_included&bit)return end;
    if(depth>=PP_INCLUDE_DEPTH) {pp_fail("include depth limit (4)");return end;}
    unsigned bytes=(unsigned)strlen(pp_headers[index].body);
    if(bytes>(unsigned)(PP_WORK-pp_work)) {pp_fail("preprocessor include work limit");return end;}
    pp_work+=(int)bytes;pp_including|=bit;pp_emit(' ');
    pp_source(pp_headers[index].body,depth+1);pp_emit(' ');
    pp_including&=~bit;if(!error_text)pp_included|=bit;
    return end;
}
static void pp_source(const char *source,int depth) {
    const char *p=source;int bol=1;
    while(*p && !error_text) {
        /* Header newlines become whitespace: lexer diagnostics retain the
           original source row, rather than jumping by hidden header lines. */
        if(*p=='\n') {pp_emit(depth?' ':'\n');p++;bol=1;continue;}
        if(pp_space((u8)*p)) {pp_emit(*p++);continue;}
        if(p[0]=='/' && p[1]=='/') {while(*p && *p!='\n')p++;pp_emit(' ');continue;}
        if(p[0]=='/' && p[1]=='*') {
            p+=2;pp_emit(' ');
            while(*p && !(p[0]=='*' && p[1]=='/')) {
                if(*p=='\n') {pp_emit(depth?' ':'\n');bol=1;}
                p++;
            }
            if(!*p) {pp_fail("unterminated comment");break;}
            p+=2;continue;
        }
        if(*p=='#') {
            if(!bol) {pp_fail("directive must start a line");break;}
            p=pp_directive(p+1,depth);continue;
        }
        bol=0;
        const char *start=p++;
        if(letter((u8)*start))while(letter((u8)*p) || digit((u8)*p))p++;
        else if(digit((u8)*start) || (*start=='.' && digit((u8)*p))) {
            while(letter((u8)*p) || digit((u8)*p) || *p=='.' ||
                 ((*p=='+' || *p=='-') && (p[-1]=='e' || p[-1]=='E' || p[-1]=='p' || p[-1]=='P')))p++;
        } else if(*start=='"' || *start=='\'') {
            while(*p && *p!='\n') {
                if(*p++==*start)break;
                if(p[-1]=='\\' && *p && *p!='\n')p++;
            }
        }
        pp_expand(start,p,0);
    }
}
static const char *preprocess(const char *source) {
    pp_count=pp_used=pp_work=0;pp_line=1;pp_included=pp_including=0;
    pp_source(source,0);pp_output[pp_used]=0;return pp_output;
}
