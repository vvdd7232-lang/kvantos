#include "textedit.h"
/* Single-threaded GUI scratch, not on the small kernel stack. Validation happens
   before committing: a full line/document or binary file never loses text. */
static char before[ED_CAP], after[ED_CAP];
static textdoc_t swap_doc;
static unsigned slen(const char *s) { unsigned n=0; while(s[n]) n++; return n; }

void text_reset(textdoc_t *d) {
    for(int r=0;r<ED_ROWS;r++) d->buf[r][0]=0;
    d->lines=1; d->cx=d->cy=d->top=d->left=d->dirty=0;
    d->ax=d->ay=d->selecting=0;
}
/* Expand tabs, remove CR; reject NUL, unsupported controls and overflow. */
static int normalize(const char *s, unsigned n, char *out) {
    unsigned used=0, row=0, col=0;
    for(unsigned i=0;i<n;i++) {
        unsigned char c=(unsigned char)s[i];
        if(c=='\r') continue;
        if(c=='\n') { row++; col=0; }
        else if(c=='\t') col+=4;
        else if(c<32 || c==127) return -1;
        else col++;
        if(row>=ED_ROWS || col>ED_COLS) return -1;
        int count=c=='\t'?4:1;
        while(count--) {
            if(used>=ED_CAP-1) return -1;
            out[used++]=c=='\t'?' ':(char)c;
        }
    }
    out[used]=0;
    return (int)used;
}
static void import(textdoc_t *d, const char *s) {
    int row=0,col=0;
    while(*s) {
        if(*s=='\n') { d->buf[row++][col]=0; col=0; }
        else d->buf[row][col++]=*s;
        s++;
    }
    d->buf[row][col]=0; d->lines=row+1;
    for(int r=row+1;r<ED_ROWS;r++) d->buf[r][0]=0;
}
int text_set(textdoc_t *d, const char *s, unsigned n) {
    if(normalize(s,n,after)<0) return 0;
    text_reset(d); import(d,after); return 1;
}
unsigned text_export(const textdoc_t *d, char *s) {
    unsigned n=0;
    for(int r=0;r<d->lines;r++) {
        for(int c=0;d->buf[r][c];c++) s[n++]=d->buf[r][c];
        if(r+1<d->lines) s[n++]='\n';
    }
    s[n]=0; return n;
}
unsigned text_offset(const textdoc_t *d, int row, int col) {
    unsigned n=(unsigned)col;
    for(int r=0;r<row;r++) n+=slen(d->buf[r])+1;
    return n;
}
void text_position(textdoc_t *d, unsigned offset) {
    d->cy=0;
    while(d->cy+1<d->lines && offset>slen(d->buf[d->cy]))
        offset-=slen(d->buf[d->cy++])+1;
    unsigned len=slen(d->buf[d->cy]);
    d->cx=(int)(offset<len?offset:len);
}
int text_selected(const textdoc_t *d) {
    return d->selecting && (d->cy!=d->ay || d->cx!=d->ax);
}
void text_range(const textdoc_t *d, unsigned *a, unsigned *b) {
    *a=*b=text_offset(d,d->cy,d->cx);
    if(text_selected(d)) {
        unsigned n=text_offset(d,d->ay,d->ax);
        if(n<*a) *a=n; else *b=n;
    }
}
int text_replace(textdoc_t *d, textundo_t *u, const char *s, unsigned n) {
    unsigned a,b,old=text_export(d,before);
    text_range(d,&a,&b);
    if(n>=ED_CAP || old-(b-a)+n>=ED_CAP) return 0;
    /* after may be the source of a load, never of this public edit API. */
    unsigned o=0;
    for(unsigned i=0;i<a;i++) after[o++]=before[i];
    for(unsigned i=0;i<n;i++) after[o++]=s[i];
    for(unsigned i=b;i<old;i++) after[o++]=before[i];
    after[o]=0;
    /* normalize in the second scratch, so failure leaves both doc and undo. */
    if(normalize(after,o,before)<0) return 0;
    unsigned inserted=0;
    for(unsigned i=0;i<n;i++) inserted+=s[i]=='\r'?0:(s[i]=='\t'?4:1);
    u->doc=*d; u->valid=1;
    import(d,before); text_position(d,a+inserted);
    d->selecting=0; d->dirty=1;
    return 1;
}
int text_next(const char *s, int col) {
    if(!s[col]) return col;
    col++;
    while(s[col] && ((unsigned char)s[col]&0xc0)==0x80) col++;
    return col;
}
int text_prev(const char *s, int col) {
    if(col<=0) return 0;
    col--;
    while(col>0 && ((unsigned char)s[col]&0xc0)==0x80) col--;
    return col;
}
int text_column(const char *s, int byte) {
    int n=0;
    for(int i=0;s[i] && i<byte;i=text_next(s,i)) n++;
    return n;
}
int text_byte(const char *s, int column) {
    int i=0;
    while(column-->0 && s[i]) i=text_next(s,i);
    return i;
}
int text_delete(textdoc_t *d, textundo_t *u, int backwards) {
    if(text_selected(d)) return text_replace(d,u,"",0);
    unsigned pos=text_offset(d,d->cy,d->cx);
    if((backwards && !pos) || (!backwards && d->cy==d->lines-1 && !d->buf[d->cy][d->cx])) return 1;
    int cy=d->cy,cx=d->cx;
    d->ay=cy; d->ax=cx; d->selecting=1;
    if(backwards) {
        if(cx) d->cx=text_prev(d->buf[cy],cx);
        else { d->cy--; d->cx=(int)slen(d->buf[d->cy]); }
    } else {
        if(d->buf[cy][cx]) d->cx=text_next(d->buf[cy],cx);
        else { d->cy++; d->cx=0; }
    }
    if(text_replace(d,u,"",0)) return 1;
    d->cy=cy; d->cx=cx; d->selecting=0; return 0;
}
int text_undo(textdoc_t *d, textundo_t *u) {
    if(!u->valid) return 0;
    swap_doc=*d; *d=u->doc; u->doc=swap_doc;
    return 1; /* one step, toggles undo / redo */
}
