/* Flat, bounded tags/aliases and ABI-specific struct layout. No aggregate ABI. */
#define RECORD_MAX 32
#define MEMBER_MAX 256
#define ALIAS_MAX 32
/* low two bits: base; next three: pointer depth; remaining bits: record id */
static int is_pointer(int t) {return t>=0 && (t&28)!=0;}
static int is_record(int t) {return t>=0 && !is_pointer(t) && (t&3)==3;}
static struct {char name[NAME_MAX];int anon,first,count,bytes,align,complete;} records[RECORD_MAX];
static struct {char name[NAME_MAX];int type,count,offset;} members[MEMBER_MAX];
static struct {char name[NAME_MAX];int type;} aliases[ALIAS_MAX];
static int nrecords,nmembers,naliases,type_pass,anon_serial,record_depth,in_function;
static int type_bytes(int t) {
    if(t<0)return 0;
    if(is_pointer(t))return MCC_WORD;
    if(is_record(t))return (t>>5)<nrecords?records[t>>5].bytes:0;
    return t==TY_CHAR?1:t==TY_VOID?0:4;
}
static int type_align(int t) {
    if(is_record(t))return (t>>5)<nrecords?records[t>>5].align:1;
    return is_pointer(t)?MCC_WORD:t==TY_INT?4:1;
}
static int align_to(int n,int alignment) {return (n+alignment-1)&~(alignment-1);}
static int alias_find(const char *name) {
    for(int i=0;i<naliases;i++)if(!strcmp(aliases[i].name,name))return i;
    return -1;
}
static int find(const char *name);
static int parse_type(void);
static int pointer_type(int t);
static int array_count(int t,int limit);
static void name_copy(char *dst);
static void type_alias(const char *name,int type) {
    int old=alias_find(name);
    if(type_pass==2 && old>=0 && aliases[old].type==type)return;
    if(old>=0 || find(name)>=0) {fail("typedef name already declared");return;}
    if(naliases==ALIAS_MAX) {fail("typedef limit (32)");return;}
    strcpy(aliases[naliases].name,name);aliases[naliases++].type=type;
}
static int record_type(void) {
    char name[NAME_MAX];int anon=-1,id=-1;
    next();
    if(token==ID)name_copy(name);
    else if(token=='{') {name[0]=0;anon=anon_serial++;}
    else {fail("expected struct tag or definition");return 3;}
    for(int i=0;i<nrecords;i++)
        if(name[0]?!strcmp(name,records[i].name):(!records[i].name[0] && records[i].anon==anon)) {id=i;break;}
    if(id<0) {
        if(nrecords==RECORD_MAX) {fail("struct tag limit (32)");return 3;}
        id=nrecords++;memset(&records[id],0,sizeof(records[id]));
        strcpy(records[id].name,name);records[id].anon=anon;records[id].align=1;
    }
    if(accept('{')) {
        if(record_depth || in_function) {fail("struct definitions must be top-level, not nested");return 3+(id<<5);}
        int replay=records[id].complete;
        if(replay && type_pass!=2) {fail("struct already defined");return 3+(id<<5);}
        int first=replay?records[id].first:nmembers, count=0, bytes=0, alignment=1;
        record_depth++;
        while(token && token!='}' && !error_text) {
            int base=parse_type();
            do {
                int type=pointer_type(base);char field[NAME_MAX];name_copy(field);
                int n=array_count(type,DATA_MAX), size=type_bytes(type), align=type_align(type);
                if(!size) {fail("field has void or incomplete type");break;}
                size*=n?n:1;bytes=align_to(bytes,align);
                if(size>DATA_MAX-bytes) {fail("struct size limit (8000 bytes)");break;}
                for(int i=0;i<count;i++)if(!strcmp(field,members[first+i].name))fail("duplicate struct field");
                if(error_text)break;
                if(replay) {
                    if(count>=records[id].count) {fail("conflicting struct definition");break;}
                    if(strcmp(field,members[first+count].name) || members[first+count].type!=type ||
                       members[first+count].count!=n || members[first+count].offset!=bytes)fail("conflicting struct layout");
                } else {
                    if(nmembers==MEMBER_MAX) {fail("struct member limit (256)");break;}
                    strcpy(members[nmembers].name,field);members[nmembers].type=type;
                    members[nmembers].count=n;members[nmembers++].offset=bytes;
                }
                count++;bytes+=size;if(align>alignment)alignment=align;
            }while(!error_text && accept(','));
            expect(';');
        }
        expect('}');record_depth--;
        bytes=align_to(bytes,alignment);
        if(!count)fail("empty structs are unsupported");
        if(bytes>DATA_MAX)fail("struct size limit (8000 bytes)");
        if(replay && count!=records[id].count)fail("conflicting struct field count");
        if(!error_text) {
            records[id].first=first;records[id].count=count;records[id].bytes=bytes;
            records[id].align=alignment;records[id].complete=1;
        }
    }
    return 3+(id<<5);
}
