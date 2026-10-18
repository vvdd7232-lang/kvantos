#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "keymap.h"
static keymap_t s;
static kbd_event_t e;
static void down(unsigned sc) { assert(keymap_feed(&s,sc,&e)); }
static void modifier(unsigned sc) { assert(!keymap_feed(&s,sc,&e)); }
int main(void) {
    keymap_init(&s); down(0x22); assert(e.key=='g' && e.codepoint=='g');
    modifier(0x38); down(0x2a); assert(e.key==KEY_LAYOUT && s.layout==1);
    modifier(0xaa); modifier(0xb8);
    down(0x22); assert(e.key=='g' && e.codepoint==0x43f && !e.modifiers);
    modifier(0x2a); down(0x22); assert(e.key=='G' && e.codepoint==0x41f && e.modifiers==KMOD_SHIFT);
    kbd_event_t snapshot=e; modifier(0xaa); assert(snapshot.modifiers==KMOD_SHIFT);
    modifier(0x3a); modifier(0xba); down(0x22); assert(e.codepoint==0x41f);
    modifier(0x36); down(0x22); assert(e.codepoint==0x43f); modifier(0xb6);
    modifier(0x3a); modifier(0xba);
    modifier(0x2a); modifier(0x36); modifier(0xaa);
    down(0x04); assert(e.key=='#' && e.codepoint==0x2116); modifier(0xb6);
    down(0x29); assert(e.codepoint==0x451); down(0x35); assert(e.codepoint=='.');
    modifier(0x1d); down(0x2e); assert(e.key==3 && !e.codepoint); modifier(0x9d);
    modifier(0xe0); modifier(0x1d); down(0x2c); assert(e.key==26 && !e.codepoint);
    modifier(0xe0); modifier(0x9d); assert(!(keymap_modifiers(&s)&KMOD_CTRL));
    modifier(0xe0); down(0x48); assert(e.key==KEY_UP && !e.codepoint);
    down(0x3f); assert(e.key==KEY_F5 && !e.codepoint);
    /* Alt+Shift in either order, latching, and both sides of modifiers. */
    modifier(0x36); modifier(0xe0); down(0x38); assert(e.key==KEY_LAYOUT && !s.layout);
    modifier(0x38); modifier(0xb8); modifier(0xe0); modifier(0xb8); modifier(0xb6);
    unsigned pause[]={0xe1,0x1d,0x45,0xe1,0x9d,0xc5};
    for(unsigned i=0;i<6;i++) modifier(pause[i]);
    assert(!keymap_modifiers(&s) && !s.skip);
    char b[5]={0}; assert(keymap_utf8(0x43f,b)==2 && !memcmp(b,"п",2));
    assert(keymap_utf8(0x2116,b)==3 && !memcmp(b,"№",3));
    assert(keymap_utf8(0x1f600,b)==4);
    assert(!keymap_utf8(0xd800,b) && !keymap_utf8(0x110000,b) && !keymap_utf8(0,b));
    modifier(0x1d); down(0x23); assert(e.key==8 && e.physical=='h');
    down(0x0e); assert(e.key==8 && e.physical==8); modifier(0x9d);
    unsigned rng=0x531a7;
    for(int i=0;i<100000;i++) {
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
        if(keymap_feed(&s,rng&255,&e)) {
            assert(e.key>=1 && e.key<=KEY_LAYOUT);
            assert(e.codepoint==0 || keymap_utf8(e.codepoint,b)>0);
            assert(!(e.modifiers&~7u));
        }
    }
    puts("Input: EN/RU, Alt+Shift, Caps/Shift sides, shortcuts, Unicode separation, UTF-8 + 100000 scan bytes PASS");
}
