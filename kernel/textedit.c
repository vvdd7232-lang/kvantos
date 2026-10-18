#include "textedit.h"
/* Single-threaded GUI scratch, not on the small kernel stack. Validation happens
   before committing: a full line/document or binary file never loses text. */
static char before[ED_CAP], after[ED_CAP], replaced[ED_CAP];
static unsigned slen(const char *s) { unsigned n=0; while(s[n]) n++; return n; }

_Static_assert(sizeof(unsigned short)==2 && ED_CAP==65536,"16-bit line offsets");
const char *text_line(const textdoc_t *d, int row) { return d->data+d->start[row]; }
/* Copy only live text/indexes, not 192 KiB of unused capacity per keystroke. */
void text_copy(textdoc_t *dst, const textdoc_t *src) {
    if(dst==src) return;
    for(unsigned i=0;i<=src->used;i++) dst->data[i]=src->data[i];
    for(int r=0;r<src->lines;r++) dst->start[r]=src->start[r];
    dst->lines=src->lines; dst->cy=src->cy; dst->cx=src->cx;
    dst->top=src->top; dst->left=src->left; dst->dirty=src->dirty;
    dst->ay=src->ay; dst->ax=src->ax; dst->selecting=src->selecting;
    dst->used=src->used; dst->width=src->width;
}
void text_reset(textdoc_t *d) {
    d->data[0]=0; d->start[0]=0; d->used=0; d->width=0;
    d->lines=1; d->cx=d->cy=d->top=d->left=d->dirty=0;
    d->ax=d->ay=d->selecting=0;
}
/* Expand tabs, remove CR; reject NUL, unsupported controls and overflow. */
static int normalize(const char *s, unsigned n, char *out) {
    unsigned used=0, col=0;
    for(unsigned i=0;i<n;i++) {
        unsigned char c=(unsigned char)s[i];
        if(c=='\r') continue;
        if(c=='\n') col=0;
        else if(c=='\t') col+=4;
        else if(c<32 || c==127) return -1;
        else col++;
        if(col>ED_COLS) return -1;
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
    unsigned n=0; int row=0;
    d->start[0]=0;
    while(s[n]) {
        d->data[n]=s[n]=='\n'?0:s[n];
        if(s[n]=='\n') d->start[++row]=(unsigned short)(n+1);
        n++;
    }
    d->data[n]=0; d->used=n; d->lines=row+1; d->width=0;
    for(int r=0;r<d->lines;r++) {
        const char *line=text_line(d,r);
        int width=text_column(line,(int)slen(line));
        if(width>d->width) d->width=width;
    }
}
int text_set(textdoc_t *d, const char *s, unsigned n) {
    if(n>=ED_CAP || normalize(s,n,after)<0) return 0;
    text_reset(d); import(d,after); return 1;
}
unsigned text_export(const textdoc_t *d, char *s) {
    for(unsigned i=0;i<d->used;i++) s[i]=d->data[i]?d->data[i]:'\n';
    s[d->used]=0; return d->used;
}
unsigned text_offset(const textdoc_t *d, int row, int col) { return d->start[row]+(unsigned)col; }
void text_position(textdoc_t *d, unsigned offset) {
    if(offset>d->used) offset=d->used;
    int low=0,high=d->lines;
    while(low<high) {
        int mid=low+(high-low)/2;
        if(d->start[mid]<=offset) low=mid+1; else high=mid;
    }
    d->cy=low?low-1:0; d->cx=(int)(offset-d->start[d->cy]);
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
static textmark_t mark(const textdoc_t *d, unsigned revision) {
    return (textmark_t){d->cy,d->cx,d->top,d->left,d->ay,d->ax,d->selecting,revision};
}
static void restore(textdoc_t *d, textmark_t m) {
    d->cy=m.cy; d->cx=m.cx; d->top=m.top; d->left=m.left;
    d->ay=m.ay; d->ax=m.ax; d->selecting=m.selecting;
}
void text_history_reset(const textdoc_t *d, textundo_t *u) {
    u->used=u->count=u->cursor=u->serial=u->current=0;
    u->saved=d->dirty?~0u:0;
}
void text_saved(textdoc_t *d, textundo_t *u) { u->saved=u->current; d->dirty=0; }
/* Called only after validation succeeds. Redo is discarded on a new branch;
   eviction changes available history, never the document or the saved marker. */
static textchange_t *history_push(textdoc_t *d, textundo_t *u, unsigned bytes) {
    if(u->serial>=~0u-1) text_history_reset(d,u); /* avoid revision wrap aliases */
    if(u->cursor<u->count) u->used=u->changes[u->cursor].data;
    u->count=u->cursor;
    while(u->count && (u->count==ED_HISTORY_STEPS || u->used+bytes>ED_HISTORY_BYTES)) {
        unsigned cut=u->changes[0].removed+u->changes[0].inserted;
        for(unsigned i=cut;i<u->used;i++) u->data[i-cut]=u->data[i];
        for(unsigned i=1;i<u->count;i++) {
            u->changes[i-1]=u->changes[i]; u->changes[i-1].data-=cut;
        }
        u->used-=cut; u->count--; u->cursor--;
    }
    textchange_t *c=&u->changes[u->count++];
    c->data=u->used; u->used+=bytes; u->cursor=u->count;
    c->before=mark(d,u->current);
    return c;
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
    if(a==b && !inserted) return 1;
    textchange_t *c=history_push(d,u,b-a+inserted);
    c->at=a; c->removed=b-a; c->inserted=inserted;
    for(unsigned i=0;i<c->removed;i++) {
        char ch=d->data[a+i]; u->data[c->data+i]=ch?ch:'\n';
    }
    for(unsigned i=0;i<inserted;i++) u->data[c->data+c->removed+i]=before[a+i];
    import(d,before); text_position(d,a+inserted);
    d->selecting=0; d->dirty=1;
    u->current=++u->serial; c->after=mark(d,u->current);
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
    if((backwards && !pos) || (!backwards && d->cy==d->lines-1 && !text_line(d,d->cy)[d->cx])) return 1;
    textmark_t original=mark(d,u->current);
    int cy=d->cy,cx=d->cx;
    d->ay=cy; d->ax=cx; d->selecting=1;
    if(backwards) {
        if(cx) d->cx=text_prev(text_line(d,cy),cx);
        else { d->cy--; d->cx=(int)slen(text_line(d,d->cy)); }
    } else {
        if(text_line(d,cy)[cx]) d->cx=text_next(text_line(d,cy),cx);
        else { d->cy++; d->cx=0; }
    }
    if(text_replace(d,u,"",0)) {
        original.revision=u->changes[u->cursor-1].before.revision;
        u->changes[u->cursor-1].before=original; return 1;
    }
    restore(d,original); return 0;
}
static int history_apply(textdoc_t *d, textundo_t *u, int redo) {
    if(redo?u->cursor==u->count:!u->cursor) return 0;
    textchange_t *c=&u->changes[redo?u->cursor:u->cursor-1];
    unsigned removed=redo?c->removed:c->inserted, inserted=redo?c->inserted:c->removed;
    const char *payload=u->data+c->data+(redo?c->removed:0);
    unsigned n=text_export(d,before),o=0;
    for(unsigned i=0;i<c->at;i++) after[o++]=before[i];
    for(unsigned i=0;i<inserted;i++) after[o++]=payload[i];
    for(unsigned i=c->at+removed;i<n;i++) after[o++]=before[i];
    after[o]=0; import(d,after);
    textmark_t m=redo?c->after:c->before; restore(d,m);
    u->current=m.revision; d->dirty=u->current!=u->saved;
    if(redo) u->cursor++; else u->cursor--;
    return 1;
}
int text_undo(textdoc_t *d, textundo_t *u) { return history_apply(d,u,0); }
int text_redo(textdoc_t *d, textundo_t *u) { return history_apply(d,u,1); }

/* Literal, case-sensitive UTF-8 search. Never select half of a character. */
static int boundary(const textdoc_t *d,unsigned at) {
    return at==d->used || ((unsigned char)d->data[at]&0xc0)!=0x80;
}
static int matches(const textdoc_t *d,const char *q,unsigned n,unsigned at) {
    if(at>d->used || n>d->used-at || !boundary(d,at) || !boundary(d,at+n)) return 0;
    for(unsigned i=0;i<n;i++) if((d->data[at+i]?d->data[at+i]:'\n')!=q[i]) return 0;
    return 1;
}
static int query_ok(const char *q,unsigned n) {
    if(!n || n>=ED_QUERY_CAP || ((unsigned char)q[0]&0xc0)==0x80) return 0;
    for(unsigned i=0;i<n;i++) if(!q[i]) return 0;
    return 1;
}
int text_find(const textdoc_t *d,const char *q,unsigned n,unsigned start,int backwards,int wrap,unsigned *found) {
    if(!query_ok(q,n) || n>d->used) return 0;
    unsigned last=d->used-n;
    if(start>d->used) start=d->used;
    if(!backwards) {
        for(unsigned i=start;i<=last;i++) if(matches(d,q,n,i)) { *found=i; return 1; }
        if(wrap) for(unsigned i=0;i<start && i<=last;i++) if(matches(d,q,n,i)) { *found=i; return 1; }
    } else {
        unsigned first=start<last?start:last;
        for(unsigned i=first+1;i;) { i--; if(matches(d,q,n,i)) { *found=i; return 1; } }
        if(wrap) for(unsigned i=last+1;i>first+1;) { i--; if(matches(d,q,n,i)) { *found=i; return 1; } }
    }
    return 0;
}
int text_replace_all(textdoc_t *d,textundo_t *u,const char *q,unsigned n,const char *s,unsigned m,unsigned *count) {
    *count=0;
    if(!query_ok(q,n) || m>=ED_QUERY_CAP) return 0;
    unsigned out=0,hits=0;
    for(unsigned i=0;i<d->used;) {
        if(matches(d,q,n,i)) {
            if(m>=ED_CAP-out) return 0;
            for(unsigned j=0;j<m;j++) replaced[out++]=s[j];
            i+=n; hits++;
        } else {
            if(out>=ED_CAP-1) return 0;
            replaced[out++]=d->data[i]?d->data[i]:'\n'; i++;
        }
    }
    replaced[out]=0;
    if(!hits) return 1;
    int same=out==d->used;
    for(unsigned i=0;same && i<out;i++) if(replaced[i]!=(d->data[i]?d->data[i]:'\n')) same=0;
    if(same) { *count=hits; return 1; }
    textmark_t original=mark(d,u->current);
    unsigned caret=text_offset(d,d->cy,d->cx);
    d->ay=d->ax=0; d->cy=d->lines-1; d->cx=(int)slen(text_line(d,d->cy)); d->selecting=1;
    if(!text_replace(d,u,replaced,out)) { restore(d,original); return 0; }
    textchange_t *change=&u->changes[u->cursor-1];
    original.revision=change->before.revision; change->before=original;
    text_position(d,caret); change->after=mark(d,u->current);
    *count=hits; return 1;
}
