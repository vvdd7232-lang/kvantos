/* Host visual regression: real GUI/framebuffer/font code, no emulated devices.
 * Device callbacks below supply deterministic data; this is NOT a boot test.
 * Function sections discard kernel paths that would perform privileged I/O. */
#include "../../kernel/fb.c"
#include "../../kernel/gui.c"

extern int open(const char *, int, ...);
extern long write(int, const void *, unsigned long);
extern int close(int);
static u32 pixels[1024 * 768];
static u8 rgb[1024 * 3];

i32 mouse_x(void) { return 130; }
i32 mouse_y(void) { return (i32)scr_h - PANEL_H - 30 - (ICON_COUNT+2)*26 + 95; }
int kbd_mousekeys(void) { return 0; }
int kapp_loaded(void) { return 0; }
u32 timer_hz(void) { return 100; }
u64 timer_ticks(void) { return 1000; }
u32 pmm_used_frames(void) { return 2048; }
void rtc_read(rtc_time_t *t) {
    memset(t, 0, sizeof(*t)); t->day=26; t->month=10; t->hour=14; t->min=35;
}
u32 pmm_total_bytes(void) { return 256*1048576; }
u32 timer_seconds(void) { return 10; }
u32 task_count(void) { return 2; }
u32 pci_count(void) { return 0; }
pci_dev_t *pci_gpu(void) { return NULL; }
const char *pci_gpu_model(u16 vid, u16 did) { return "Test adapter"; }
void heap_stats(u32 *t,u32 *u,u32 *b) { *t=8192; *u=4096; *b=1; }
int vbe_can_modeset(void) { return 0; }
void vbe_current(vmode_t *m) { memset(m,0,sizeof(*m)); m->width=scr_w; m->height=scr_h; m->bpp=32; }
u32 vbe_vram_bytes(void) { return 8*1048576; }
u32 vbe_mode_count(void) { return 0; }
int vbe_mode_get(u32 i,vmode_t *m) { return -1; }
u32 vbe_get_refresh(void) { return 60; }
void cpu_vendor(char *s) { strcpy(s, "GenuineIntel"); }
void cpu_brand(char *s) { strcpy(s, "Example x86 processor with a long model name"); }

static int save(const char *path) {
    int fd = open(path, 577, 0644); /* O_WRONLY | O_CREAT | O_TRUNC */
    if (fd < 0) return 1;
    char header[64];
    ksnprintf(header, sizeof(header), "P6\n%u %u\n255\n", scr_w, scr_h);
    int n = (int)strlen(header);
    if (write(fd, header, n) != n) return 2;
    for (u32 y=0; y<scr_h; y++) {
        for (u32 x=0; x<scr_w; x++) {
            u32 c=pixels[y*scr_w+x];
            rgb[x*3]=c>>16; rgb[x*3+1]=c>>8; rgb[x*3+2]=c;
        }
        if (write(fd, rgb, scr_w*3) != scr_w*3) return 3;
    }
    return close(fd);
}

static void init(u32 w, u32 h) {
    scr_w=fb_w=w; scr_h=fb_h=h; fb_pitch=w*4; fb_bpp=32; fb_ok=1;
    fb_hw=fb_draw=(u8 *)pixels;
    win_count=0; memset(wins,0,sizeof(wins));
    launcher_open=0; sel_icon=hover_icon=-1;
    palette_init(); fb_clear_clip(); hit_reset();
}

#define CHECK(x) do { if (!(x)) { \
    char fail[256]; \
    ksnprintf(fail,sizeof(fail),"GUI check failed at line %u: %s\n",(u32)__LINE__,#x); \
    write(2,fail,strlen(fail)); return 1; \
} } while (0)
int main(int argc, char **argv) {
    CHECK(argc==2);
    CHECK(theme_accent==1 && theme_wall==5 && WALL_COUNT==6);
    char path[256];
    for(int lang=0;lang<2;lang++) {
        kv_lang_set(lang ? KV_LANG_RU : KV_LANG_EN);
        init(1024,768);
        draw_desktop(); draw_panel();
        ksnprintf(path,sizeof(path),"%s/desktop-%s.ppm",argv[1],lang?"ru":"en");
        CHECK(!save(path));
        int id=win_open(app_name(APP_HELP),APP_HELP,282,58,600,440,C_ACCENT);
        window_t *v=&wins[id];
        draw_window_frame(v,0); app_help(v,v->x+1,v->y+TITLE_H+1,v->w-2);
        id=win_open(app_name(APP_ABOUT),APP_ABOUT,450,410,420,300,C_ACCENT);
        v=&wins[id];
        draw_window_frame(v,1); app_about(v,v->x+1,v->y+TITLE_H+1,v->w-2);
        draw_panel();
        ksnprintf(path,sizeof(path),"%s/windows-%s.ppm",argv[1],lang?"ru":"en");
        CHECK(!save(path));
        id=win_open(app_name(APP_SETTINGS),APP_SETTINGS,294,180,470,430,C_ACCENT);
        v=&wins[id]; set_tab=1;
        draw_window_frame(v,1); app_settings(v,v->x+1,v->y+TITLE_H+1,v->w-2);
        draw_panel();
        ksnprintf(path,sizeof(path),"%s/appearance-%s.ppm",argv[1],lang?"ru":"en");
        CHECK(!save(path));
        launcher_open=1; draw_panel(); draw_launcher();
        ksnprintf(path,sizeof(path),"%s/menu-%s.ppm",argv[1],lang?"ru":"en");
        CHECK(!save(path));
    }
    /* Every wallpaper/accent combination at minimum and normal resolution;
       catch bounds issues under ASan/UBSan, including legacy Matrix settings. */
    for(int size=0;size<2;size++) {
        init(size?1024:640,size?768:480);
        for(int a=0;a<ACCENT_COUNT;a++) for(int wall=0;wall<WALL_COUNT;wall++) {
            theme_accent=a; theme_wall=wall; palette_init(); draw_desktop();
            for(int i=0;i<ICON_COUNT;i++) {
                CHECK(icons[i].x>=0 && icons[i].y>=0);
                CHECK(icons[i].y+58<(i32)scr_h-PANEL_H);
                CHECK(icon_at(icons[i].x+12,icons[i].y+12)==i);
            }
            launcher_open=1; draw_panel(); draw_launcher();
            sel_icon=hover_icon=0; draw_tooltip();
            for(int aa=0;aa<2;aa++) {
                fb_set_aa(aa);
                window_t v={.x=310,.y=42,.w=310,.h=230};
                strcpy(v.title,app_name(APP_SETTINGS));
                draw_window_frame(&v,aa);
                widget_chip(0,1,325,92,160,26,T("Apply","Применить"),aa);
                widget_check(0,2,325,130,T("Window shadows","Тени под окнами"),aa);
                fb_set_aa(1);
            }
            hit_reset();
        }
    }
    /* Caption hit regions agree with the painted buttons, excluding borders. */
    window_t frame={.x=100,.y=50,.w=400,.h=300};
    CHECK(window_caption_at(&frame,487,61)==CAPTION_CLOSE);
    CHECK(window_caption_at(&frame,461,61)==CAPTION_MINIMIZE);
    CHECK(window_caption_at(&frame,475,61)==CAPTION_NONE);
    CHECK(window_caption_at(&frame,461,70)==CAPTION_NONE);
    for (int size=0;size<2;size++) for (int seconds=0;seconds<2;seconds++) {
        init(size?1024:640,size?768:480); opt_seconds=seconds;
        CHECK(taskbar_window_at(20)==-1);
        for (int n=1;n<=MAX_WIN;n++) {
            win_open("Window",APP_ABOUT,20,30,400,300,C_ACCENT);
            int step=taskbar_step();
            for (int i=0;i<n;i++) {
                int x=94+i*step;
                CHECK(taskbar_window_at(x+(step-6)/2)==z_order[i]);
                CHECK(taskbar_window_at(x+step-5)==-1);
            }
            CHECK(94+n*step<=(i32)scr_w-(seconds?148:124));
            CHECK(taskbar_window_at((i32)scr_w-20)==-1);
            draw_panel();
        }
        for(int lang=0;lang<2;lang++) {
            kv_lang_set(lang?KV_LANG_RU:KV_LANG_EN);
            hit_reset();
            window_t *v=&wins[0]; v->x=10; v->y=8; v->w=470; v->h=430;
            draw_window_frame(v,1); set_tab=1;
            app_settings(v,v->x+1,v->y+TITLE_H+1,v->w-2);
            CHECK(hit_count==3+ACCENT_COUNT+WALL_COUNT+3);
            for(int i=0;i<hit_count;i++) {
                hit_t *h=&hits[i];
                CHECK(h->x>v->x && h->x+h->w<v->x+v->w);
                CHECK(h->y>v->y+TITLE_H && h->y+h->h<v->y+v->h);
            }
        }
    }
    /* Pixel regressions: captions/chips must be symmetric and gradients
       must never tint x-1 / x+w outside the requested rectangle. */
    init(640,480);
    u32 sentinel=fb_rgb(217,17,199);
    for (int aa=0;aa<2;aa++) for(int size=0;size<4;size++) {
        i32 x=20,y=20,w=19+size,h=15+size;
        fb_set_aa(aa); fb_clear(sentinel);
        fb_round_fill_aa(x,y,w,h,4,C_WHITE);
        for(i32 j=0;j<h;j++) for(i32 i=0;i<w;i++) {
            u32 c=pixels[(y+j)*scr_w+x+i];
            CHECK(c==pixels[(y+j)*scr_w+x+w-1-i]);
            CHECK(c==pixels[(y+h-1-j)*scr_w+x+i]);
        }
        for(int gradient=0;gradient<2;gradient++) {
            fb_clear(sentinel);
            fb_gradient_v_round(x,y,w,h,gradient?4:0,30,80,150,200,220,240);
            for(i32 j=0;j<(i32)scr_h;j++) for(i32 i=0;i<(i32)scr_w;i++) {
                if(i<x || i>=x+w || j<y || j>=y+h)
                    CHECK(pixels[j*scr_w+i]==sentinel);
                else CHECK(pixels[j*scr_w+i]==pixels[j*scr_w+x+w-1-(i-x)]);
            }
        }
    }
    fb_set_aa(1); fb_clear(sentinel);
    fb_line_aa(20,20,26,26,C_WHITE);
    CHECK(pixels[20*scr_w+20]==C_WHITE && pixels[26*scr_w+26]==C_WHITE);
    CHECK(pixels[27*scr_w+26]==sentinel);
    fb_line_aa(20,26,26,20,C_WHITE);
    CHECK(pixels[26*scr_w+20]==C_WHITE && pixels[20*scr_w+26]==C_WHITE);

    /* Long window titles must not paint over caption buttons. */
    window_t narrow={.x=20,.y=20,.w=200,.h=140};
    u32 buttons[50*TITLE_H];
    fb_clear(sentinel); draw_window_frame(&narrow,1);
    for(int j=0;j<TITLE_H;j++) for(int i=0;i<50;i++)
        buttons[j*50+i]=pixels[(20+j)*scr_w+170+i];
    strncpy(narrow.title,"A deliberately long window title",sizeof(narrow.title));
    fb_clear(sentinel); draw_window_frame(&narrow,1);
    for(int j=0;j<TITLE_H;j++) for(int i=0;i<50;i++)
        CHECK(buttons[j*50+i]==pixels[(20+j)*scr_w+170+i]);

    /* Strict UTF-8 fitting, including 0..3-cell labels and long tokens. */
    const char *samples[]={"Application title that is much too long", "Очень длинное русское название"};
    for(int i=0;i<2;i++) for(u32 cols=0;cols<40;cols++) {
        char text[256]; fit_name(text,sizeof(text),samples[i],cols);
        CHECK(utf8_len(text)<=cols);
        const char *src=samples[i];
        while(wrap_next(&src,text,sizeof(text),cols?cols:1)) CHECK(utf8_len(text)<=(cols?cols:1));
    }
    char tiny[2]; fit_name(tiny,sizeof(tiny),"Тест",4); CHECK(!tiny[0]);
    /* These draw outside a widget in the old unbounded text path. */
    for(int aa=0;aa<2;aa++) {
        fb_set_aa(aa); fb_clear(sentinel); hit_reset();
        widget_chip(0,1,40,40,64,25,samples[1],0);
        for(i32 j=0;j<(i32)scr_h;j++) for(i32 i=0;i<(i32)scr_w;i++)
            if(i<40 || i>=104 || j<40 || j>=65) CHECK(pixels[j*scr_w+i]==sentinel);
    }
    fb_set_aa(1);

    /* Real Help/About sizes, both languages, every page; assert that the
       client content never draws onto the title, border or desktop. */
    theme_accent=1; theme_wall=5; opt_seconds=0;
    for(int lang=0;lang<2;lang++) {
        kv_lang_set(lang?KV_LANG_RU:KV_LANG_EN);
        init(640,480);
        int id=win_open(app_name(APP_HELP),APP_HELP,10,8,600,440,C_ACCENT);
        window_t *v=&wins[id];
        for(int page=0;page<3;page++) {
            fb_clear(sentinel); hit_reset(); v->scroll=page;
            app_help(v,v->x+1,v->y+TITLE_H+1,v->w-2);
            for(i32 j=0;j<(i32)scr_h;j++) for(i32 i=0;i<(i32)scr_w;i++)
                if(i<=v->x || i>=v->x+v->w-1 || j<=v->y+TITLE_H || j>=v->y+v->h-1)
                    CHECK(pixels[j*scr_w+i]==sentinel);
            if(!page) {
                ksnprintf(path,sizeof(path),"%s/help-small-%s.ppm",argv[1],lang?"ru":"en");
                draw_desktop(); draw_window_frame(v,1); app_help(v,v->x+1,v->y+TITLE_H+1,v->w-2);
                draw_panel(); CHECK(!save(path));
            }
        }
        v->scroll=0; hit_reset(); app_help(v,v->x+1,v->y+TITLE_H+1,v->w-2);
        CHECK(help_click(id,v->x+v->w-60,v->y+v->h-24)); CHECK(v->scroll==1);
        CHECK(help_click(id,v->x+60,v->y+v->h-24)); CHECK(v->scroll==0);
        CHECK(help_click(id,v->x+60,v->y+v->h-24)); CHECK(v->scroll==0);
        i32 pages=(help_text(v,0,0,v->w-2,0)+help_visible_rows(v)-1)/help_visible_rows(v);
        for(int i=0;i<pages+1;i++) CHECK(help_click(id,v->x+v->w-60,v->y+v->h-24));
        CHECK(v->scroll==pages-1);
        v->w=420; v->h=300; v->app=APP_ABOUT; strcpy(v->title,app_name(APP_ABOUT)); fb_clear(sentinel);
        app_about(v,v->x+1,v->y+TITLE_H+1,v->w-2);
        for(i32 j=0;j<(i32)scr_h;j++) for(i32 i=0;i<(i32)scr_w;i++)
            if(i<=v->x || i>=v->x+v->w-1 || j<=v->y+TITLE_H || j>=v->y+v->h-1)
                CHECK(pixels[j*scr_w+i]==sentinel);
        ksnprintf(path,sizeof(path),"%s/about-small-%s.ppm",argv[1],lang?"ru":"en");
        draw_desktop(); draw_window_frame(v,1); app_about(v,v->x+1,v->y+TITLE_H+1,v->w-2);
        draw_panel(); CHECK(!save(path));
    }
    return 0;
}
