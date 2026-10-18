#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "textedit.h"
static textdoc_t d, saved;
static textundo_t undo, saved_undo;
static char out[ED_CAP], input[ED_CAP];
static void set(const char *s) { assert(text_set(&d,s,(unsigned)strlen(s))); text_history_reset(&d,&undo); }
static void same(const char *s) { text_export(&d,out); assert(!strcmp(out,s)); }
static void selection(int ay,int ax,int cy,int cx) { d.ay=ay;d.ax=ax;d.cy=cy;d.cx=cx;d.selecting=1; }
static unsigned rng=0x21b197u;
static unsigned random32(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;return rng; }
static void search_tests(void) {
    unsigned at,count;
    set("foo\nfoo foo"); saved=d; saved_undo=undo;
    assert(text_find(&d,"foo",3,0,0,0,&at) && at==0);
    assert(text_find(&d,"foo",3,1,0,0,&at) && at==4);
    assert(text_find(&d,"foo",3,10,0,1,&at) && at==0);
    assert(text_find(&d,"foo",3,3,1,0,&at) && at==0);
    assert(text_find(&d,"foo",3,0,1,1,&at) && at==0);
    assert(text_find(&d,"foo\nf",5,0,0,0,&at) && at==0);
    assert(!text_find(&d,"FOO",3,0,0,1,&at) && !text_find(&d,"",0,0,0,1,&at));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    selection(1,5,0,1); d.top=1;
    assert(text_replace_all(&d,&undo,"foo",3,"бар",6,&count) && count==3); same("бар\nбар бар");
    assert(undo.count==1 && d.dirty); assert(text_undo(&d,&undo)); same("foo\nfoo foo");
    assert(d.ay==1 && d.ax==5 && d.cy==0 && d.cx==1 && d.top==1 && d.selecting && !d.dirty);
    assert(text_redo(&d,&undo)); same("бар\nбар бар");
    text_saved(&d,&undo); saved=d; saved_undo=undo;
    assert(text_replace_all(&d,&undo,"бар",6,"бар",6,&count) && count==3);
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    assert(text_replace_all(&d,&undo,"нет",6,"",0,&count) && count==0);
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    assert(text_find(&d,"бар",6,1,0,0,&at) && at==7);
    assert(!text_find(&d,"\xd0",1,0,0,1,&at)); /* no partial UTF-8 */
    assert(!text_find(&d,"\xb1",1,0,0,1,&at));
    set("aaaa"); assert(text_replace_all(&d,&undo,"aa",2,"b",1,&count) && count==2); same("bb");
    assert(text_replace_all(&d,&undo,"b",1,"",0,&count) && count==2); same("");
    set("a"); selection(0,0,0,1); saved=d; saved_undo=undo;
    memset(input,'x',121); input[121]=0;
    assert(!text_replace_all(&d,&undo,"a",1,input,121,&count));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    assert(!text_replace_all(&d,&undo,"a",1,"\001",1,&count));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    memset(input,'\n',ED_CAP-1); input[ED_CAP-1]=0; set(input); saved=d; saved_undo=undo;
    assert(!text_replace_all(&d,&undo,"\n",1,"\n\n",2,&count));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    assert(text_replace_all(&d,&undo,"\n",1,"",0,&count) && count==ED_CAP-1); same("");
    assert(text_undo(&d,&undo)); same(input); assert(text_redo(&d,&undo)); same("");
    /* Differential literal/non-overlapping replacement against an independent loop. */
    for(int t=0;t<3000;t++) {
        char q[8],rep[8],expected[512]; unsigned len=random32()%81,n=random32()%7+1,m=random32()%7;
        for(unsigned i=0;i<len;i++) input[i]="abc\n"[random32()%4];
        input[len]=0; set(input);
        for(unsigned i=0;i<n;i++) q[i]="abc\n"[random32()%4];
        for(unsigned i=0;i<m;i++) rep[i]="abc\n"[random32()%4];
        unsigned outn=0,hits=0;
        for(unsigned i=0;i<len;) {
            if(n<=len-i && !memcmp(input+i,q,n)) { memcpy(expected+outn,rep,m);outn+=m;i+=n;hits++; }
            else expected[outn++]=input[i++];
        }
        expected[outn]=0; saved=d; saved_undo=undo;
        int valid=1,cols=0;
        for(unsigned i=0;i<outn;i++) { if(expected[i]=='\n') cols=0; else if(++cols>ED_COLS) valid=0; }
        int ok=text_replace_all(&d,&undo,q,n,rep,m,&count);
        assert(ok==valid);
        if(ok) {
            assert(count==hits); same(expected);
            if(strcmp(input,expected)) { assert(undo.count==1); assert(text_undo(&d,&undo));same(input);assert(text_redo(&d,&undo));same(expected); }
        } else assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    }
    puts("Find/replace: wrap, backwards, UTF-8, multiline, one Undo, no-op, failure atomicity, 65535 replacements + 3000 differential cases PASS");
}
int main(void) {
    search_tests();
    text_reset(&d); same(""); assert(d.lines==1);
    set("abcd\nefgh\nlast"); selection(2,2,0,2);
    unsigned a,b; text_range(&d,&a,&b); assert(a==2 && b==12);
    assert(text_replace(&d,&undo,"X\nY",3)); same("abX\nYst");
    assert(d.cy==1 && d.cx==1 && d.dirty && !text_selected(&d));
    assert(text_undo(&d,&undo)); same("abcd\nefgh\nlast");
    assert(text_redo(&d,&undo)); same("abX\nYst");
    set("alpha\nbeta"); selection(0,0,1,4); assert(text_replace(&d,&undo,"",0)); same("");
    assert(d.lines==1 && d.cx==0 && d.cy==0);
    assert(text_undo(&d,&undo)); same("alpha\nbeta");
    set("a\nb"); d.cy=1; assert(text_delete(&d,&undo,1)); same("ab");
    assert(d.cx==1 && d.cy==0);
    set("a\nb"); d.cx=1; assert(text_delete(&d,&undo,0)); same("ab");
    set("Привет\nмир"); assert(text_byte(text_line(&d,0),3)==6 && text_column(text_line(&d,0),6)==3);
    d.cx=6; assert(text_delete(&d,&undo,1)); same("Првет\nмир");
    assert(text_undo(&d,&undo)); same("Привет\nмир");
    set("a\r\nb\tc"); same("a\nb    c");
    set("keep"); saved=d;
    assert(!text_set(&d,"a\0b",3) && !memcmp(&d,&saved,sizeof(d)));
    assert(!text_set(&d,"\001",1) && !memcmp(&d,&saved,sizeof(d)));
    memset(input,'x',121); input[121]=0;
    assert(!text_set(&d,input,121)); same("keep");
    input[120]=0; set(input); d.cx=120; saved=d; saved_undo=undo;
    assert(!text_replace(&d,&undo,"a",1));
    assert(!memcmp(&saved,&d,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    assert(text_replace(&d,&undo,"\n",1)); assert(d.lines==2);
    memset(input,'x',120); memcpy(input+120,"\ny",3); set(input); d.cx=120;
    saved=d; assert(!text_delete(&d,&undo,0)); same("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\ny");
    assert(d.cy==saved.cy && d.cx==saved.cx && d.lines==2);
    /* Crossing the old threshold is an ordinary successful edit. */
    memset(input,'\n',127); input[127]=0; set(input); d.cy=127;
    assert(text_replace(&d,&undo,"\n",1)); assert(d.lines==129);
    assert(text_undo(&d,&undo) && d.lines==128);
    assert(text_redo(&d,&undo) && d.lines==129);
    /* No separate row ceiling: one index exists for each possible byte. */
    memset(input,'\n',ED_CAP-1); input[ED_CAP-1]=0; set(input);
    assert(d.lines==ED_CAP && d.used==ED_CAP-1 && d.width==0);
    assert(text_export(&d,out)==ED_CAP-1 && !strcmp(out,input));
    text_position(&d,ED_CAP-1); assert(d.cy==ED_CAP-1 && d.cx==0);
    saved=d; saved_undo=undo;
    assert(!text_replace(&d,&undo,"\n",1));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    assert(!text_set(&d,input,ED_CAP) && !memcmp(&d,&saved,sizeof(d)));
    for(unsigned i=0;i<ED_CAP;i++) {
        text_position(&d,i); assert(text_offset(&d,d.cy,d.cx)==i);
    }
    selection(0,0,ED_CAP-1,0); assert(text_replace(&d,&undo,"short",5)); same("short");
    assert(d.width==5); assert(text_undo(&d,&undo) && d.lines==ED_CAP);
    d.selecting=0; text_position(&d,ED_CAP-1);
    assert(text_delete(&d,&undo,1)); assert(d.lines==ED_CAP-1);
    /* Full byte pool with nonempty 120-byte lines; replace it transactionally. */
    for(unsigned i=0;i<ED_CAP-1;i++) input[i]=i%121==120?'\n':'x';
    input[ED_CAP-1]=0; set(input); assert(d.width==120);
    assert(text_export(&d,out)==ED_CAP-1 && !strcmp(out,input));
    selection(0,0,d.lines-1,(int)strlen(text_line(&d,d.lines-1)));
    assert(text_replace(&d,&undo,"short",5)); same("short");
    assert(text_undo(&d,&undo)); same(input);
    assert(text_redo(&d,&undo)); same("short");
    /* Tabs can overflow the byte pool without exceeding any line width. */
    set("keep"); saved=d; saved_undo=undo;
    for(unsigned i=0;i<40000;i++) input[i]=i%2?'\n':'\t';
    assert(!text_set(&d,input,40000) && !memcmp(&d,&saved,sizeof(d)));
    assert(!text_replace(&d,&undo,input,40000));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    set("a\nб"); text_position(&d,d.used);
    assert(text_replace(&d,&undo,"в",2)); assert(text_replace(&d,&undo,"г",2));
    assert(text_undo(&d,&undo)); same("a\nбв");
    assert(text_undo(&d,&undo)); same("a\nб");
    assert(text_redo(&d,&undo)); assert(text_redo(&d,&undo)); same("a\nбвг");
    /* Three edits, independent directions, saved marker and redo branching. */
    set("");
    assert(text_replace(&d,&undo,"a",1));
    assert(text_replace(&d,&undo,"b",1));
    assert(text_replace(&d,&undo,"c",1)); same("abc");
    assert(undo.cursor==3 && undo.count==3);
    assert(text_undo(&d,&undo)); same("ab");
    assert(text_undo(&d,&undo)); same("a");
    assert(text_redo(&d,&undo)); same("ab");
    text_saved(&d,&undo); assert(!d.dirty);
    assert(text_undo(&d,&undo) && d.dirty); same("a");
    assert(text_redo(&d,&undo) && !d.dirty); same("ab");
    assert(text_undo(&d,&undo));
    assert(text_replace(&d,&undo,"X",1)); same("aX");
    assert(d.dirty && !text_redo(&d,&undo));
    assert(text_undo(&d,&undo)); same("a"); assert(d.dirty);
    assert(text_redo(&d,&undo)); same("aX");
    text_saved(&d,&undo);
    assert(text_undo(&d,&undo) && d.dirty);
    assert(text_redo(&d,&undo) && !d.dirty);
    /* A failed edit after Undo must not discard the still-valid redo branch. */
    set("");
    assert(text_replace(&d,&undo,"a",1)); assert(text_replace(&d,&undo,"b",1));
    assert(text_replace(&d,&undo,"c",1)); assert(text_undo(&d,&undo));
    saved=d; saved_undo=undo;
    assert(text_replace(&d,&undo,"",0));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    memset(input,'x',121);
    assert(!text_replace(&d,&undo,input,121));
    assert(!memcmp(&d,&saved,sizeof(d)) && !memcmp(&undo,&saved_undo,sizeof(undo)));
    assert(text_redo(&d,&undo)); same("abc");
    /* History has a count budget; editing continues while oldest edits expire. */
    set("");
    for(int i=0;i<80;i++) { char ch=(char)('a'+i%26); assert(text_replace(&d,&undo,&ch,1)); }
    assert(undo.count==ED_HISTORY_STEPS && undo.cursor==ED_HISTORY_STEPS);
    for(int i=0;i<ED_HISTORY_STEPS;i++) assert(text_undo(&d,&undo));
    assert(d.used==16 && d.dirty && !text_undo(&d,&undo));
    for(int i=0;i<ED_HISTORY_STEPS;i++) assert(text_redo(&d,&undo));
    assert(d.used==80 && !text_redo(&d,&undo));
    /* Byte budget can hold a full-document exchange; subsequent edits evict it. */
    for(unsigned i=0;i<ED_CAP-1;i++) input[i]=i%121==120?'\n':'x';
    input[ED_CAP-1]=0; set(input);
    for(unsigned i=0;i<ED_CAP-1;i++) if(input[i]=='x') input[i]='y';
    selection(0,0,d.lines-1,(int)strlen(text_line(&d,d.lines-1)));
    assert(text_replace(&d,&undo,input,ED_CAP-1));
    assert(undo.count==1 && undo.used==ED_HISTORY_BYTES-2);
    assert(text_undo(&d,&undo) && text_line(&d,0)[0]=='x');
    assert(text_redo(&d,&undo)); same(input);
    int last=d.lines-1,len=(int)strlen(text_line(&d,last));
    selection(last,len-1,last,len); assert(text_replace(&d,&undo,"z",1));
    assert(undo.used==ED_HISTORY_BYTES && undo.count==2);
    selection(0,0,0,1); assert(text_replace(&d,&undo,"w",1));
    assert(undo.count==2 && undo.used==4);
    assert(text_undo(&d,&undo)); assert(text_undo(&d,&undo)); same(input);
    assert(!text_undo(&d,&undo));
    /* Saved state and dirty initial documents survive reset and serial rollover. */
    set("base"); d.dirty=1; text_history_reset(&d,&undo); d.cx=4;
    assert(text_replace(&d,&undo,"!",1)); assert(text_undo(&d,&undo) && d.dirty);
    text_saved(&d,&undo); undo.serial=~0u-1; d.selecting=0; d.cx=4;
    assert(text_replace(&d,&undo,"?",1)); assert(text_undo(&d,&undo) && !d.dirty); same("base");
    set("");
    static char check[ED_CAP];
    for(int trial=0;trial<25000;trial++) {
        saved=d; saved_undo=undo;
        unsigned length=text_export(&d,out);
        unsigned first=random32()%(length+1),last=random32()%(length+1);
        text_position(&d,first); d.ay=d.cy; d.ax=d.cx;
        text_position(&d,last); d.selecting=1;
        saved=d;
        unsigned count=random32()%100;
        for(unsigned i=0;i<count;i++) input[i]=(random32()%15==0)?'\n':(char)('a'+random32()%26);
        int ok=text_replace(&d,&undo,input,count);
        if(!ok) { assert(!memcmp(&d,&saved,sizeof(d))); assert(!memcmp(&undo,&saved_undo,sizeof(undo))); }
        else if(trial%7==0 && undo.serial!=saved_undo.serial && undo.cursor) {
            text_export(&d,check); text_export(&saved,input);
            assert(text_undo(&d,&undo)); same(input);
            assert(text_redo(&d,&undo)); same(check);
        }
        assert(undo.cursor<=undo.count && undo.count<=ED_HISTORY_STEPS && undo.used<=ED_HISTORY_BYTES);
        assert(d.lines>=1 && d.lines<=ED_CAP && d.cy>=0 && d.cy<d.lines);
        assert(d.cx>=0 && d.cx<=(int)strlen(text_line(&d,d.cy)));
        for(int r=0;r<d.lines;r++) assert(strlen(text_line(&d,r))<=ED_COLS);
        length=text_export(&d,out); assert(length<ED_CAP && length==d.used);
        int width=0;
        for(int r=0;r<d.lines;r++) {
            assert(text_offset(&d,r,0)==(unsigned)(text_line(&d,r)-d.data));
            int w=text_column(text_line(&d,r),(int)strlen(text_line(&d,r)));
            if(w>width) width=w;
        }
        assert(width==d.width);
        if(trial%101==0) { text_reset(&d); text_history_reset(&d,&undo); }
    }
    puts("Text editors: selection, clipboard replacement, UTF-8, 64-step undo/redo, saved marker, branch/eviction, 129/65536 lines, byte limits, atomic failure + 25000 edits PASS");
}
