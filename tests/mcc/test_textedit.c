#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "textedit.h"
static textdoc_t d, saved;
static textundo_t undo, saved_undo;
static char out[ED_CAP], input[ED_CAP];
static void set(const char *s) { assert(text_set(&d,s,(unsigned)strlen(s))); undo.valid=0; }
static void same(const char *s) { text_export(&d,out); assert(!strcmp(out,s)); }
static void selection(int ay,int ax,int cy,int cx) { d.ay=ay;d.ax=ax;d.cy=cy;d.cx=cx;d.selecting=1; }
static unsigned rng=0x21b197u;
static unsigned random32(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;return rng; }
int main(void) {
    text_reset(&d); same(""); assert(d.lines==1);
    set("abcd\nefgh\nlast"); selection(2,2,0,2);
    unsigned a,b; text_range(&d,&a,&b); assert(a==2 && b==12);
    assert(text_replace(&d,&undo,"X\nY",3)); same("abX\nYst");
    assert(d.cy==1 && d.cx==1 && d.dirty && !text_selected(&d));
    assert(text_undo(&d,&undo)); same("abcd\nefgh\nlast");
    assert(text_undo(&d,&undo)); same("abX\nYst");
    set("alpha\nbeta"); selection(0,0,1,4); assert(text_replace(&d,&undo,"",0)); same("");
    assert(d.lines==1 && d.cx==0 && d.cy==0);
    assert(text_undo(&d,&undo)); same("alpha\nbeta");
    set("a\nb"); d.cy=1; assert(text_delete(&d,&undo,1)); same("ab");
    assert(d.cx==1 && d.cy==0);
    set("a\nb"); d.cx=1; assert(text_delete(&d,&undo,0)); same("ab");
    set("Привет\nмир"); assert(text_byte(d.buf[0],3)==6 && text_column(d.buf[0],6)==3);
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
    set("x\ny"); memset(d.buf[0],'x',120); d.buf[0][120]=0; d.cx=120;
    saved=d; assert(!text_delete(&d,&undo,0)); same("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\ny");
    assert(d.cy==saved.cy && d.cx==saved.cx && d.lines==2);
    memset(input,'\n',127); input[127]=0; set(input); d.cy=127;
    saved=d; assert(!text_replace(&d,&undo,"\n",1)); assert(!memcmp(&d,&saved,sizeof(d)));
    /* Fill every row to the byte limit, then replace the entire document. */
    unsigned n=0;
    for(int r=0;r<ED_ROWS;r++) { for(int c=0;c<ED_COLS;c++) input[n++]='x'; if(r+1<ED_ROWS) input[n++]='\n'; }
    input[n]=0; set(input); assert(text_export(&d,out)==ED_CAP-1);
    selection(0,0,127,120); assert(text_replace(&d,&undo,"short",5)); same("short");
    set("");
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
        assert(d.lines>=1 && d.lines<=ED_ROWS && d.cy>=0 && d.cy<d.lines);
        assert(d.cx>=0 && d.cx<=(int)strlen(d.buf[d.cy]));
        for(int r=0;r<d.lines;r++) assert(strlen(d.buf[r])<=ED_COLS);
        length=text_export(&d,out); assert(length<ED_CAP);
        if(trial%101==0) { text_reset(&d); undo.valid=0; }
    }
    puts("Text editors: selection, clipboard replacement, UTF-8, undo/redo, limits, atomic failure + 25000 edits PASS");
}
