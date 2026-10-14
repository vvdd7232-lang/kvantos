/* Mini C compiler for the in-system workshop. Emits native i386/x86-64 machine code,
   not interpreted bytecode. No heap, FPU, host compiler, assembler or linker.
   Deliberately a documented subset, not an ISO C implementation. */
#include "mcc.h"
#include "kvapp.h"
#ifdef MCC_HOSTTEST
#include <string.h>
#endif

#define CODE_MAX 24000
#define DATA_MAX 8000
#define SYM_MAX 256
#define LOCAL_MAX 128
#define FIX_MAX 1024
#define ARG_MAX 8
#define NAME_MAX 48

enum { ID = 256, NUM, STR, INT, CHAR, VOID, CONST, IF, ELSE, WHILE, FOR,
       RETURN, BREAK, CONTINUE, EQ, NE, LE, GE, SHL, SHR, AND, OR,
       INC, DEC, ADDSET, SUBSET, MULSET, DIVSET, MODSET };
enum { TY_INT, TY_CHAR, TY_VOID }; /* adding 4 adds one pointer indirection */
enum { GLOBAL, FUNCTION, BUILTIN };
typedef struct {
    char name[NAME_MAX];
    int kind, type, argc, args[ARG_MAX], defined, address;
} symbol_t;
typedef struct { char name[NAME_MAX]; int type, offset; } local_t;
typedef struct { int where, in_data, target, function, relative; } fix_t;
typedef struct { int type, lvalue; } expr_t;
typedef struct { int again, first_break; } loop_t;

/* Compiler workspace is BSS: the kernel task stack is only 8 KiB. */
static u8 code[CODE_MAX], data[DATA_MAX];
static symbol_t symbols[SYM_MAX];
static local_t locals[LOCAL_MAX];
static fix_t fixes[FIX_MAX];
static loop_t loops[8];
static int breaks[256];
static int nc, nd, ns, nl, nf, nb, nloop, local_bytes, depth, return_type;
static const char *input;
static int token, line, token_line, tokens, error_line;
static u32 value;
static char word[NAME_MAX], literal[512];
static int literal_len;
static const char *error_text;

static void fail(const char *message) {
    if (!error_text) { error_text = message; error_line = token_line; }
}
static int enter(void) {
    if (error_text) return 0;
    if (++depth > 24) { fail("nesting limit (24)"); depth--; return 0; }
    return !error_text;
}
static int letter(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int digit(int c) { return c >= '0' && c <= '9'; }
static int escape(void) {
    int c = (u8)*input++;
    if (c == 'n') return '\n';
    if (c == 'r') return '\r';
    if (c == 't') return '\t';
    if (c == '0') return 0;
    if (c == '\\' || c == '\'' || c == '"') return c;
    fail("unsupported escape");
    return c;
}
static void next(void) {
    if (error_text) { token = 0; return; }
    if (++tokens > 24000) { fail("too many tokens"); token = 0; return; }
    for (;;) {
        while (*input == ' ' || *input == '\t' || *input == '\r' || *input == '\n')
            if (*input++ == '\n') line++;
        if (input[0] == '/' && input[1] == '/') {
            while (*input && *input != '\n') input++;
        } else if (input[0] == '/' && input[1] == '*') {
            input += 2;
            while (*input && !(input[0] == '*' && input[1] == '/'))
                if (*input++ == '\n') line++;
            if (!*input) { fail("unterminated comment"); token = 0; return; }
            input += 2;
        } else break;
    }
    token_line = line;
    token = (u8)*input;
    if (!token) return;
    if (letter(token)) {
        int n = 0;
        while (letter((u8)*input) || digit((u8)*input)) {
            if (n < NAME_MAX - 1) word[n++] = *input;
            else fail("identifier too long");
            input++;
        }
        word[n] = 0;
        static const struct { const char *name; int id; } keywords[] = {
            {"int",INT}, {"char",CHAR}, {"void",VOID}, {"const",CONST},
            {"if",IF}, {"else",ELSE}, {"while",WHILE}, {"for",FOR},
            {"return",RETURN}, {"break",BREAK}, {"continue",CONTINUE}
        };
        token = ID;
        for (unsigned i = 0; i < sizeof(keywords)/sizeof(keywords[0]); i++)
            if (!strcmp(word, keywords[i].name)) { token = keywords[i].id; break; }
        return;
    }
    if (digit(token)) {
        u32 base = 10;
        value = 0;
        if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) { base = 16; input += 2; }
        else if (input[0] == '0' && digit(input[1])) base = 8;
        int n = 0;
        while (digit(*input) || (base == 16 && ((*input >= 'a' && *input <= 'f') || (*input >= 'A' && *input <= 'F')))) {
            u32 d = digit(*input) ? (u32)(*input - '0') : (u32)((*input | 32) - 'a' + 10);
            if (d >= base || value > (0xFFFFFFFFu - d) / base) fail("integer literal out of range");
            value = value * base + d; input++; n++;
        }
        if (!n) fail("expected digits");
        if (*input == 'u' || *input == 'U') input++; /* arithmetic remains signed int */
        token = NUM;
        return;
    }
    if (token == '"' || token == '\'') {
        int quote = *input++;
        literal_len = 0;
        while (*input && *input != quote && *input != '\n') {
            int c = (u8)*input++;
            if (c == '\\') { if (!*input) break; c = escape(); }
            if (literal_len < (int)sizeof(literal) - 1) literal[literal_len++] = (char)c;
            else fail("literal too long (511 bytes)");
        }
        if (*input != quote) fail("unterminated literal");
        else input++;
        literal[literal_len] = 0;
        token = STR;
        if (quote == '\'') {
            if (literal_len != 1) fail("expected one character");
            value = (u8)literal[0]; token = NUM;
        }
        return;
    }
    static const struct { const char *s; int id; } pairs[] = {
        {"==",EQ},{"!=",NE},{"<=",LE},{">=",GE},{"<<",SHL},{">>",SHR},
        {"&&",AND},{"||",OR},{"++",INC},{"--",DEC},{"+=",ADDSET},{"-=",SUBSET},
        {"*=",MULSET},{"/=",DIVSET},{"%=",MODSET}
    };
    for (unsigned i = 0; i < sizeof(pairs)/sizeof(pairs[0]); i++)
        if (input[0] == pairs[i].s[0] && input[1] == pairs[i].s[1]) {
            token = pairs[i].id; input += 2; return;
        }
    input++;
    if (token == '#') fail("preprocessor not supported; built-in API needs no include");
}
static int accept(int t) { if (token != t) return 0; next(); return 1; }
static void expect(int t) { if (!accept(t)) fail("unexpected token"); }
static int is_type(void) { return token == INT || token == CHAR || token == VOID || token == CONST; }
static int parse_type(void) {
    accept(CONST);
    int t = TY_INT;
    if (token == INT) t = TY_INT;
    else if (token == CHAR) t = TY_CHAR;
    else if (token == VOID) t = TY_VOID;
    else { fail("expected int, char or void"); return t; }
    next();
    return t;
}
static int pointer_type(int t) {
    while (accept('*')) { t += 4; if (t > 18) fail("pointer depth limit"); accept(CONST); }
    return t;
}
static void name_copy(char *dst) {
    if (token != ID) { fail("expected name"); dst[0] = 0; return; }
    /* word always fits NAME_MAX; unlike libc strncpy the kernel terminates. */
    strcpy(dst, word); next();
}
static void byte(int b) { if (nc >= CODE_MAX) fail("code limit (24000 bytes)"); else code[nc++] = (u8)b; }
static void put32(u8 *dst, int at, u32 v) {
    if (error_text) return;
    for (int i = 0; i < 4; i++) dst[at + i] = (u8)(v >> (8 * i));
}
static void imm(u32 n) { for (int i = 0; i < 4; i++) byte(n >> (8 * i)); }
static int reserve_data(int bytes) {
    int at = nd;
    if (bytes < 0 || bytes > DATA_MAX - nd) fail("data limit (8000 bytes)");
    else { memset(data + nd, 0, (size_t)bytes); nd += bytes; }
    return at;
}
static int string_data(const char *s, int bytes) {
    int at = reserve_data(bytes);
    if (!error_text) memcpy(data + at, s, (size_t)bytes);
    return at;
}
static void fix(int where, int in_data, int target, int function, int relative) {
    if (nf >= FIX_MAX) fail("too many relocations");
    else fixes[nf++] = (fix_t){where, in_data, target, function, relative};
}
static void wide(int enabled) { if (MCC_TARGET64 && enabled) byte(0x48); }
/* Pointer offsets are signed C ints, widened before address arithmetic. */
static void scale(int type, int reg) {
    int step = type == TY_CHAR + 4 ? 1 : type >= 8 ? MCC_WORD : 4;
    if (MCC_TARGET64) { byte(0x48); byte(0x63); byte(reg ? 0xC9 : 0xC0); }
    if (step > 1) { wide(1); byte(0xC1); byte(reg ? 0xE1 : 0xE0); byte(step == 8 ? 3 : 2); }
}
static void address(int data_offset) { byte(0xB8); fix(nc,0,data_offset,0,0); imm(0); }
static int jump(int condition) {
    if (condition) { byte(0x0F); byte(condition); } else byte(0xE9);
    int at = nc; imm(0); return at;
}
static void patch(int at, int target) { if (!error_text) put32(code, at, (u32)(target - at - 4)); }
static void jump_to(int target) { int at = jump(0); patch(at, target); }
static void test(void) { wide(1); byte(0x85); byte(0xC0); }
static int find(const char *name) {
    for (int i = 0; i < ns; i++) if (!strcmp(name, symbols[i].name)) return i;
    return -1;
}
static int symbol(const char *name, int kind, int type) {
    if (ns >= SYM_MAX) { fail("too many symbols"); return 0; }
    symbol_t *s = &symbols[ns];
    memset(s,0,sizeof(*s)); strcpy(s->name,name); s->kind=kind; s->type=type; s->address=-1;
    return ns++;
}
static void local(const char *name, int type, int offset) {
    if (nl >= LOCAL_MAX) { fail("too many locals"); return; }
    if (type == TY_VOID) fail("variable cannot be void");
    for (int i = nl - 1; i >= 0; i--) if (!strcmp(name,locals[i].name)) {
        /* Block shadowing is deliberately not supported. */
        fail("duplicate local name"); break;
    }
    strcpy(locals[nl].name,name); locals[nl].type=type; locals[nl++].offset=offset;
}
static void rvalue(expr_t *e) {
    if (e->lvalue) {
        if (e->type == TY_CHAR) { byte(0x0F); byte(0xBE); byte(0x00); }
        else { wide(e->type >= 4); byte(0x8B); byte(0x00); }
        e->lvalue = 0;
    }
}
static void store(int type) { /* address in ecx, value in eax */
    wide(type >= 4); byte(type == TY_CHAR ? 0x88 : 0x89); byte(0x01);
    if (type == TY_CHAR) { byte(0x0F); byte(0xBE); byte(0xC0); }
}
static int stride(int type) { return type == TY_CHAR + 4 ? 1 : type >= 8 ? MCC_WORD : 4; }
static expr_t expression(int minimum);
static expr_t unary(void);

static expr_t change(expr_t e, int op, int post) {
    if (!e.lvalue) fail("increment needs a variable");
    byte(0x50); rvalue(&e);
    wide(e.type >= 4); byte(0x89); byte(0xC2); /* old value */
    wide(e.type >= 4); byte(0x83); byte(op == INC ? 0xC0 : 0xE8); byte(e.type >= 4 ? stride(e.type) : 1);
    byte(0x59); store(e.type);
    if (post) { wide(e.type >= 4); byte(0x89); byte(0xD0); }
    return e;
}
static expr_t primary(void) {
    expr_t e = {TY_INT,0};
    if (token == NUM) { byte(0xB8); imm(value); next(); }
    else if (token == STR) {
        e.type = TY_CHAR + 4;
        address(string_data(literal, literal_len + 1)); next();
    } else if (accept('(')) { e = expression(1); expect(')'); }
    else if (token == ID) {
        char name[NAME_MAX]; name_copy(name);
        if (accept('(')) {
            int s = find(name), argc = 0;
            if (s < 0 || symbols[s].kind == GLOBAL) { fail("unknown function"); return e; }
            if (token != ')') do {
                expr_t arg = expression(1); rvalue(&arg); byte(0x50);
                if (++argc > ARG_MAX) { fail("at most 8 arguments"); break; }
            } while (accept(','));
            expect(')');
            if (symbols[s].argc >= 0 ? argc != symbols[s].argc : argc < -symbols[s].argc)
                fail("wrong argument count");
            /* Evaluation is left-to-right; reverse the pushed words for our stack calling convention. */
            for (int i = 0; i < argc / 2; i++) {
                wide(1); byte(0x8B); byte(0x44); byte(0x24); byte(i*MCC_WORD);
                wide(1); byte(0x8B); byte(0x54); byte(0x24); byte((argc-1-i)*MCC_WORD);
                wide(1); byte(0x89); byte(0x54); byte(0x24); byte(i*MCC_WORD);
                wide(1); byte(0x89); byte(0x44); byte(0x24); byte((argc-1-i)*MCC_WORD);
            }
            if (symbols[s].kind == BUILTIN) {
                if (MCC_TARGET64) {
                    /* Private stack ABI -> SysV, preserving the original stack
                       even inside nested expressions and odd-sized frames. */
                    static const int regs[] = {0x7C,0x74,0x54,0x4C,0x44,0x4C};
                    for (int i=0; i<argc && i<6; i++) {
                        byte(i<4 ? 0x48 : 0x4C); byte(0x8B);
                        byte(regs[i]); byte(0x24); byte(i*8);
                    }
                    byte(0x49); byte(0x89); byte(0xE3); /* r11 = argument stack */
                    byte(0x53); wide(1); byte(0x89); byte(0xE3); /* save rbx, rbx=rsp */
                    wide(1); byte(0x83); byte(0xE4); byte(0xF0); /* align rsp */
                    if (argc>6) {
                        wide(1); byte(0x83); byte(0xEC); byte(16);
                        for(int i=6;i<argc;i++) {
                            byte(0x49); byte(0x8B); byte(0x43); byte(i*8);
                            wide(1); byte(0x89); byte(0x44); byte(0x24); byte((i-6)*8);
                        }
                    }
                    address(0); wide(1); byte(0x8B); byte(0x00);
                    byte(0x4C); byte(0x8B); byte(0x90); imm((u32)symbols[s].address);
                    byte(0x31); byte(0xC0); /* SysV variadic: no vector arguments */
                    byte(0x41); byte(0xFF); byte(0xD2); /* call r10 */
                    wide(1); byte(0x89); byte(0xDC); byte(0x5B); /* rsp=rbx; pop rbx */
                } else {
                    byte(0xA1); fix(nc,0,0,0,0); imm(0);
                    byte(0xFF); byte(0x90); imm((u32)symbols[s].address);
                }
            } else { byte(0xE8); fix(nc,0,s,1,1); imm(0); }
            if (argc) { wide(1); byte(0x81); byte(0xC4); imm((u32)argc*MCC_WORD); }
            e.type = symbols[s].type;
        } else {
            int l;
            for (l = nl-1; l >= 0; l--) if (!strcmp(name,locals[l].name)) break;
            if (l >= 0) {
                wide(1); byte(0x8D); byte(0x85); imm((u32)locals[l].offset); e.type=locals[l].type;
            } else {
                int s = find(name);
                if (s < 0 || symbols[s].kind != GLOBAL) { fail("unknown variable"); return e; }
                address(symbols[s].address); e.type=symbols[s].type;
            }
            e.lvalue = 1;
        }
    } else { fail("expected expression"); return e; }
    while (!error_text) {
        if (accept('[')) {
            rvalue(&e);
            if (e.type < 4) { fail("index needs a pointer"); break; }
            byte(0x50);
            expr_t index = expression(1); rvalue(&index); expect(']');
            scale(e.type,0);
            byte(0x59); wide(1); byte(0x01); byte(0xC8);
            e.type -= 4; e.lvalue = 1;
        } else if (token == INC || token == DEC) { int op=token; next(); e=change(e,op,1); }
        else break;
    }
    return e;
}
static expr_t unary(void) {
    expr_t e = {TY_INT,0};
    if (!enter()) return e;
    int op = token;
    if (op == '+' || op == '-' || op == '!' || op == '~' || op == '*' || op == '&' || op == INC || op == DEC) {
        next(); e = unary();
        if (op == '&') {
            if (!e.lvalue) fail("address-of needs a variable");
            e.type += 4; e.lvalue=0;
        } else if (op == INC || op == DEC) e=change(e,op,0);
        else {
            rvalue(&e);
            if (op == '*') { if (e.type < 4) fail("dereference needs a pointer"); e.type-=4; e.lvalue=1; }
            if (op == '-') { byte(0xF7); byte(0xD8); }
            if (op == '~') { byte(0xF7); byte(0xD0); }
            if (op == '!') { test(); byte(0x0F); byte(0x94); byte(0xC0); byte(0x0F); byte(0xB6); byte(0xC0); e.type=TY_INT; }
        }
    } else e=primary();
    depth--; return e;
}
static int precedence(int t) {
    switch(t) {
        case '=': case ADDSET: case SUBSET: case MULSET: case DIVSET: case MODSET: return 1;
        case OR:return 2; case AND:return 3; case '|':return 4; case '^':return 5; case '&':return 6;
        case EQ:case NE:return 7; case '<':case '>':case LE:case GE:return 8;
        case SHL:case SHR:return 9; case '+':case '-':return 10; case '*':case '/':case '%':return 11;
        default:return 0;
    }
}
static void binary(int op, int pointers) { /* lhs ecx, rhs eax */
    switch (op) {
        case '+':wide(pointers);byte(0x01);byte(0xC8);break;
        case '-':wide(pointers);byte(0x29);byte(0xC1);wide(pointers);byte(0x89);byte(0xC8);break;
        case '*':byte(0x0F);byte(0xAF);byte(0xC1);break;
        case '/':case '%':
            byte(0x89);byte(0xC2);byte(0x89);byte(0xC8);byte(0x89);byte(0xD1);
            byte(0x99);byte(0xF7);byte(0xF9);
            if(op=='%'){byte(0x89);byte(0xD0);} break;
        case '&':byte(0x21);byte(0xC8);break; case '|':byte(0x09);byte(0xC8);break;
        case '^':byte(0x31);byte(0xC8);break;
        case SHL:case SHR:
            byte(0x89);byte(0xC2);byte(0x89);byte(0xC8);byte(0x89);byte(0xD1);
            byte(0xD3);byte(op==SHL?0xE0:0xF8);break;
        default: {
            wide(pointers);byte(0x39);byte(0xC1);
            int cc=op==EQ?0x94:op==NE?0x95:op=='<'?0x9C:op=='>'?0x9F:op==LE?0x9E:0x9D;
            byte(0x0F);byte(cc);byte(0xC0);byte(0x0F);byte(0xB6);byte(0xC0);break;
        }
    }
}
static expr_t expression(int minimum) {
    expr_t left = {TY_INT,0};
    if (!enter()) return left;
    left=unary();
    while (!error_text && precedence(token) >= minimum) {
        int op=token, prec=precedence(op); next();
        if (prec==1) {
            if (!left.lvalue) { fail("assignment needs a variable"); break; }
            byte(0x50);
            if (op!='=') { rvalue(&left); byte(0x50); }
            expr_t right=expression(1); rvalue(&right);
            if (op!='=') {
                byte(0x59);
                int base=op==ADDSET?'+':op==SUBSET?'-':op==MULSET?'*':op==DIVSET?'/':'%';
                if (left.type>=4 && (base=='+' || base=='-')) scale(left.type,0);
                binary(base,left.type>=4);
            }
            byte(0x59); store(left.type); left.lvalue=0;
        } else if (op==AND || op==OR) {
            rvalue(&left); test(); int short_jump=jump(op==AND?0x84:0x85);
            expr_t right=expression(prec+1); rvalue(&right);
            test(); byte(0x0F);byte(0x95);byte(0xC0);byte(0x0F);byte(0xB6);byte(0xC0);
            int end=jump(0); patch(short_jump,nc); byte(0xB8);imm(op==OR);patch(end,nc);
            left=(expr_t){TY_INT,0};
        } else {
            rvalue(&left); byte(0x50);
            expr_t right=expression(prec+1); rvalue(&right); byte(0x59);
            int lp=left.type>=4, rp=right.type>=4;
            if ((op=='+' || op=='-') && lp && !rp) scale(left.type,0);
            if (op=='+' && !lp && rp) scale(right.type,1);
            binary(op,lp||rp);
            if (op=='-' && lp && rp) {
                if(stride(left.type)>1){wide(1);byte(0xC1);byte(0xF8);byte(stride(left.type)==8?3:2);}
                left.type=TY_INT;
            } else if(op=='+' && !lp && rp) left.type=right.type;
            else if(prec<10 || (!lp && !rp)) left.type=TY_INT;
            left.lvalue=0;
        }
    }
    depth--; return left;
}
static void discard_expression(void) { expr_t e=expression(1);rvalue(&e); }
static void declarations(void) {
    int base=parse_type();
    do {
        int t=pointer_type(base); char name[NAME_MAX];name_copy(name);
        local_bytes+=MCC_WORD;
        if(local_bytes>LOCAL_MAX*MCC_WORD)fail("local stack limit");
        local(name,t,-local_bytes);
        if (accept('=')) {
            expr_t e=expression(1);rvalue(&e);
            wide(1);byte(0x8D);byte(0x8D);imm((u32)-local_bytes); store(t);
        }
    } while (!error_text && accept(','));
    expect(';');
}
static void statement(void);
static void loop_body(int again, int exit) {
    if (nloop>=8) {fail("loop nesting limit (8)");return;}
    loops[nloop++]=(loop_t){again,nb};
    statement(); jump_to(again); patch(exit,nc);
    int start=loops[--nloop].first_break;
    while(nb>start) patch(breaks[--nb],nc);
}
static void statement(void) {
    if(!enter()) return;
    if(accept('{')) {
        int scope=nl;
        while(!error_text && token && token!='}') {
            if(is_type()) declarations(); else statement();
        }
        expect('}');nl=scope;
    } else if(accept(IF)) {
        expect('(');discard_expression();expect(')');test();int no=jump(0x84);
        statement();
        if(accept(ELSE)){int end=jump(0);patch(no,nc);statement();patch(end,nc);}else patch(no,nc);
    } else if(accept(WHILE)) {
        int again=nc;expect('(');discard_expression();expect(')');test();int exit=jump(0x84);
        loop_body(again,exit);
    } else if(accept(FOR)) {
        int scope=nl;expect('(');
        if(is_type())declarations();else {if(token!=';')discard_expression();expect(';');}
        int cond=nc;
        if(token!=';')discard_expression();else {byte(0xB8);imm(1);}
        expect(';');test();int exit=jump(0x84), body=jump(0), again=nc;
        if(token!=')')discard_expression();
        expect(')');jump_to(cond);patch(body,nc);
        loop_body(again,exit);nl=scope;
    } else if(accept(RETURN)) {
        if(token!=';') {
            expr_t result=expression(1);rvalue(&result);
            if((return_type==TY_VOID)!=(result.type==TY_VOID))fail("return type mismatch");
        } else {
            if(return_type!=TY_VOID)fail("return needs a value");
            byte(0x31);byte(0xC0);
        }
        if(return_type==TY_CHAR){byte(0x0F);byte(0xBE);byte(0xC0);}
        expect(';');byte(0xC9);byte(0xC3);
    } else if(token==BREAK || token==CONTINUE) {
        int op=token;next();expect(';');
        if(!nloop)fail("break/continue outside loop");
        else if(op==CONTINUE)jump_to(loops[nloop-1].again);
        else if(nb>=256)fail("too many breaks");else breaks[nb++]=jump(0);
    } else if(!accept(';')) {discard_expression();expect(';');}
    depth--;
}

/* Normalize API slots to the selected target, also for host tests. */
#define SLOT(field) (MCC_WORD + ((offsetof(kv_api_t,field)-offsetof(kv_api_t,width))/sizeof(void *))*MCC_WORD)
static void builtins(void) {
    static const struct {const char *name;int argc,type,offset;} list[]={
#define API(name,count,type) {#name,count,type,SLOT(name)}
        API(width,0,TY_INT),API(height,0,TY_INT),API(rgb,3,TY_INT),
        API(clear,1,TY_VOID),API(pixel,3,TY_VOID),API(fill,5,TY_VOID),API(rect,5,TY_VOID),
        API(line,5,TY_VOID),API(text,5,TY_VOID),API(text_width,1,TY_INT),API(status,1,TY_VOID),
        API(alloc,1,TY_VOID+4),API(release,1,TY_VOID),API(ticks,0,TY_INT),API(hz,0,TY_INT),
        API(seconds,0,TY_INT),API(clock,3,TY_VOID),API(beep,2,TY_VOID),
        API(file_read,3,TY_INT),API(file_write,3,TY_INT),API(file_delete,1,TY_INT),
        API(file_list,3,TY_INT),API(file_exists,1,TY_INT),API(mem_set,3,TY_VOID+4),
        API(mem_copy,3,TY_VOID+4),API(str_len,1,TY_INT),API(str_cmp,2,TY_INT),
        API(str_copy,3,TY_VOID),API(format,-3,TY_VOID),API(random,0,TY_INT),API(log,1,TY_VOID),API(lang,0,TY_INT),API(delta_ms,0,TY_INT)
#undef API
    };
    for(unsigned i=0;i<sizeof(list)/sizeof(list[0]);i++){
        int n=symbol(list[i].name,BUILTIN,list[i].type);
        symbols[n].argc=list[i].argc;symbols[n].address=list[i].offset;
    }
}
static void translation_unit(int pass) {
    while(token && !error_text) {
        int base=parse_type();
        do {
            int type=pointer_type(base);char name[NAME_MAX];name_copy(name);
            int s=find(name);
            if(accept('(')) {
                int argc=0, argtypes[ARG_MAX]; char argnames[ARG_MAX][NAME_MAX];
                if(token!=')')do{
                    if(argc>=ARG_MAX){fail("at most 8 parameters");break;}
                    int param_type=parse_type();
                    if(param_type==TY_VOID && token==')' && !argc)break;
                    argtypes[argc]=pointer_type(param_type);name_copy(argnames[argc]);argc++;
                }while(!error_text && accept(','));
                expect(')');
                if(pass==1){
                    int previous = s >= 0;
                    if(s<0)s=symbol(name,FUNCTION,type);
                    if(symbols[s].kind!=FUNCTION || symbols[s].type!=type)fail("conflicting declaration");
                    if(previous && symbols[s].argc!=argc)fail("conflicting parameter count");
                    for(int i=0; previous && i<argc; i++)
                        if(symbols[s].args[i]!=argtypes[i])fail("conflicting parameter type");
                    symbols[s].argc=argc;
                    for(int i=0;i<argc;i++)symbols[s].args[i]=argtypes[i];
                }
                if(accept(';')) break;
                if(token!='{'){fail("expected function body");break;}
                if(pass==1){
                    if(symbols[s].defined)fail("function already defined");
                    symbols[s].defined=1;
                    int braces=0;
                    do {if(token=='{')braces++;if(token=='}')braces--;next();}while(token && braces && !error_text);
                    if(braces)fail("unterminated function");
                }else{
                    symbols[s].address=nc;nl=local_bytes=nloop=nb=0;return_type=type;
                    for(int i=0;i<argc;i++)local(argnames[i],argtypes[i],MCC_WORD*2+i*MCC_WORD);
                    byte(0x55);wide(1);byte(0x89);byte(0xE5);wide(1);byte(0x81);byte(0xEC);int frame=nc;imm(0);
                    statement();put32(code,frame,(u32)local_bytes);
                    byte(0x31);byte(0xC0);byte(0xC9);byte(0xC3);
                }
                break;
            } else {
                if(type==TY_VOID)fail("global cannot be void");
                if(pass==1){
                    if(s>=0)fail("name already declared");
                    else {
                        s=symbol(name,GLOBAL,type);
                        while((nd&(MCC_WORD-1)) && !error_text)reserve_data(1);
                        symbols[s].address=reserve_data(MCC_WORD);
                    }
                }
                if(accept('=')){
                    int negative=accept('-');accept('+');
                    if(token==STR && type>=4 && !negative){
                        if(pass==1){int at=string_data(literal,literal_len+1);fix(symbols[s].address,1,at,0,0);}
                        next();
                    }else{
                        if(token!=NUM)fail("global initializer must be a literal");
                        if(pass==1 && !error_text)put32(data,symbols[s].address,negative?0u-value:value);
                        next();
                    }
                }
                if(!accept(',')){expect(';');break;}
            }
        }while(!error_text);
    }
}
static void reset_lexer(const char *source){input=source;line=token_line=1;tokens=depth=0;next();}
const char *mcc_error(void){return error_text?error_text:"";}
int mcc_error_line(void){return error_line;}

int mcc_compile(const char *source,const char *title,u8 *output,u32 capacity,u32 *size){
    error_text=NULL;error_line=token_line=1;nc=nd=ns=nl=nf=nb=nloop=depth=0;
    if(size)*size=0;
    if(!source || !output || !size){fail("invalid compiler arguments");return -1;}
    u32 length=0;while(source[length] && length<=MCC_SOURCE_MAX)length++;
    if(length>MCC_SOURCE_MAX){fail("source limit (16000 bytes)");return -1;}
    reserve_data(MCC_TARGET64 ? 72 : 40); /* saved API + architecture-native descriptor */
    int title_len=0;while(title && title[title_len] && title_len<31)title_len++;
    int title_at=reserve_data(title_len+1);
    if(title_len)memcpy(data+title_at,title,(size_t)title_len);
    fix(MCC_WORD,1,title_at,0,0);put32(data,2*MCC_WORD,480);put32(data,2*MCC_WORD+4,300);
    builtins();reset_lexer(source);translation_unit(1);
    /* kapp_main(api): save API, return the generated application descriptor. */
    if (MCC_TARGET64) {
        address(0);wide(1);byte(0x89);byte(0x38); /* saved API = rdi */
    } else {
        byte(0x8B);byte(0x44);byte(0x24);byte(4);
        byte(0xA3);fix(nc,0,0,0,0);imm(0);
    }
    address(MCC_WORD);byte(0xC3);
    reset_lexer(source);translation_unit(2);
    static const struct{const char *name;int at,argc;} callbacks[]={
        {"open",16,0},{"draw",20,0},{"key",24,1},{"click",28,3},{"tick",32,0},{"close",36,0}
    };
    int has_entry=0;
    for(unsigned i=0;i<sizeof(callbacks)/sizeof(callbacks[0]);i++){
        int s=find(callbacks[i].name);
        if(s<0 && i==0)s=find("main");
        if(s<0)continue;
        if(symbols[s].kind!=FUNCTION || symbols[s].argc!=callbacks[i].argc)fail("invalid callback signature");
        int at = MCC_TARGET64 ? 24+(int)i*8 : callbacks[i].at;
        if (MCC_TARGET64) {
            /* SysV event handlers enter a private stack-argument function. */
            int wrapper = nc;
            for(int a=callbacks[i].argc-1;a>=0;a--) byte(a==0?0x57:a==1?0x56:0x52);
            byte(0xE8);fix(nc,0,s,1,1);imm(0);
            if(callbacks[i].argc){wide(1);byte(0x83);byte(0xC4);byte(callbacks[i].argc*8);}
            byte(0xC3);
            put32(data,at,KAPP_LOAD_BASE+(u32)wrapper);
        } else fix(at,1,s,1,0);
        if(i<=1)has_entry=1;
    }
    while((nc&(MCC_WORD-1)) && !error_text)byte(0x90);
    if(!has_entry)fail("define void draw(void) or int main(void)");
    for(int i=0;i<nf && !error_text;i++){
        fix_t *f=&fixes[i];u32 target;
        if(f->function){
            symbol_t *s=&symbols[f->target];
            if(!s->defined || s->address<0){fail("function declared but not defined");break;}
            target=KAPP_LOAD_BASE+(u32)s->address;
        }else target=KAPP_LOAD_BASE+(u32)nc+(u32)f->target;
        if(f->relative)target-=KAPP_LOAD_BASE+(u32)f->where+4;
        put32(f->in_data?data:code,f->where,target);
    }
    u32 total=64+(u32)nc+(u32)nd;
    if(total>capacity)fail("output buffer too small");
    if(error_text)return -1;
    kapp_header_t header;
    memset(&header,0,sizeof(header));memcpy(header.magic,"KAPP",4);
    header.version=KAPP_FORMAT_VERSION;header.header_size=64;header.api_version=KV_API_VERSION;
    header.flags=KAPP_FLAG_WINDOW | (MCC_TARGET64 ? KAPP_FLAG_ARCH64 : 0);header.load_base=header.entry=KAPP_LOAD_BASE;
    header.code_size=(u32)nc+(u32)nd;
    if(title_len)memcpy(header.name,title,(size_t)title_len);
    memcpy(output,&header,64);memcpy(output+64,code,(size_t)nc);memcpy(output+64+nc,data,(size_t)nd);
    *size=total;return 0;
}
