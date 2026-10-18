#include "keymap.h"
/* PS/2 set 1; hardware-independent and sanitizer-tested. */
static const char lower[128]={
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0
};
static const char upper[128]={
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0
};
static const unsigned short russian[128]={
    [0x10]=0x439,[0x11]=0x446,[0x12]=0x443,[0x13]=0x43a,
    [0x14]=0x435,[0x15]=0x43d,[0x16]=0x433,[0x17]=0x448,
    [0x18]=0x449,[0x19]=0x437,[0x1a]=0x445,[0x1b]=0x44a,
    [0x1e]=0x444,[0x1f]=0x44b,[0x20]=0x432,[0x21]=0x430,
    [0x22]=0x43f,[0x23]=0x440,[0x24]=0x43e,[0x25]=0x43b,
    [0x26]=0x434,[0x27]=0x436,[0x28]=0x44d,[0x29]=0x451,
    [0x2c]=0x44f,[0x2d]=0x447,[0x2e]=0x441,[0x2f]=0x43c,
    [0x30]=0x438,[0x31]=0x442,[0x32]=0x44c,[0x33]=0x431,[0x34]=0x44e
};
void keymap_init(keymap_t *s) { *s=(keymap_t){0}; }
unsigned keymap_modifiers(const keymap_t *s) {
    return (s->pressed&3?KMOD_SHIFT:0)|(s->pressed&12?KMOD_CTRL:0)|(s->pressed&48?KMOD_ALT:0);
}
static int navigation(unsigned sc) {
    switch(sc) {
        case 0x48:return KEY_UP; case 0x50:return KEY_DOWN;
        case 0x4b:return KEY_LEFT; case 0x4d:return KEY_RIGHT;
        case 0x47:return KEY_HOME; case 0x4f:return KEY_END;
        case 0x49:return KEY_PGUP; case 0x51:return KEY_PGDN;
        case 0x53:return KEY_DEL; case 0x52:return KEY_INS;
    }
    return 0;
}
int keymap_feed(keymap_t *s, unsigned scan, kbd_event_t *e) {
    if(scan>255 || scan==0xfa || scan==0xfe || scan==0xee || !scan || scan==0xff) return 0;
    if(s->skip) { s->skip--; return 0; }
    if(scan==0xe1) { s->skip=5; s->extended=0; return 0; } /* Pause */
    if(scan==0xe0) { s->extended=1; return 0; }
    int ext=s->extended,release=(scan&0x80)!=0;
    unsigned sc=scan&0x7f,bit=0;
    s->extended=0;
    if(sc==0x2a && !ext) bit=1;
    if(sc==0x36 && !ext) bit=2;
    if(sc==0x1d) bit=ext?8:4;
    if(sc==0x38) bit=ext?32:16;
    if(sc==0x3a && !ext) bit=64;
    if(bit) {
        int already=(s->pressed&bit)!=0;
        if(release) s->pressed&=~bit; else s->pressed|=bit;
        if(bit==64 && !release && !already) s->caps=!s->caps;
        unsigned mods=keymap_modifiers(s);
        int combo=(mods&(KMOD_ALT|KMOD_SHIFT))==(KMOD_ALT|KMOD_SHIFT);
        int changed=combo && !s->combo;
        s->combo=combo;
        if(changed) { s->layout=!s->layout; *e=(kbd_event_t){KEY_LAYOUT,0,mods,0}; return 1; }
        return 0;
    }
    if(release) return 0;
    unsigned mods=keymap_modifiers(s),cp=0;
    int key=navigation(sc);
    if(ext) {
        if(sc==0x1c) key='\n';
        if(sc==0x35) { key='/'; cp='/'; }
        if(!key) return 0;
    } else if(sc>=0x3b && sc<=0x44) key=KEY_F1+(int)(sc-0x3b);
    else if(!key) {
        unsigned lo=(unsigned char)lower[sc];
        if(!lo) return 0;
        int shifted=(mods&KMOD_SHIFT)!=0;
        int up=(lo>='a'&&lo<='z')?(shifted^s->caps):shifted;
        key=(unsigned char)(up?upper[sc]:lower[sc]);
        cp=(unsigned)key;
        if(s->layout) {
            if(russian[sc]) {
                cp=russian[sc];
                if(shifted^s->caps) cp=cp==0x451?0x401:cp-0x20;
            } else if(sc==0x35) cp=shifted?',':'.';
            else if(sc==0x2b) cp=shifted?'/':'\\';
            else if(shifted) {
                if(sc==3) cp='"';
                if(sc==4) cp=0x2116;
                if(sc==5) cp=';';
                if(sc==7) cp=':';
                if(sc==8) cp='?';
            }
        }
    }
    unsigned physical=(unsigned)key;
    if(mods&KMOD_CTRL) {
        if(key>='a' && key<='z') key=key-'a'+1;
        else if(key>='A' && key<='Z') key=key-'A'+1;
    }
    if(key<32 || key>=127 || mods&(KMOD_CTRL|KMOD_ALT)) cp=0;
    *e=(kbd_event_t){key,cp,mods,physical}; return 1;
}
unsigned keymap_utf8(unsigned c,char out[4]) {
    if(!c || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return 0;
    if(c<0x80) { out[0]=(char)c; return 1; }
    if(c<0x800) { out[0]=(char)(0xc0|(c>>6)); out[1]=(char)(0x80|(c&63)); return 2; }
    if(c<0x10000) {
        out[0]=(char)(0xe0|(c>>12)); out[1]=(char)(0x80|((c>>6)&63)); out[2]=(char)(0x80|(c&63)); return 3;
    }
    out[0]=(char)(0xf0|(c>>18)); out[1]=(char)(0x80|((c>>12)&63));
    out[2]=(char)(0x80|((c>>6)&63)); out[3]=(char)(0x80|(c&63)); return 4;
}
