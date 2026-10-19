/* CPL3 transition/diagnostics and opt-in format2/ABI4 event runtime.
   Legacy ABI2/3 remains a separate explicitly trusted ring0 path. */
#include "kernel.h"
#include "user_space.h"
#include "user_service.h"
#include "user_image.h"
#include "user_app.h"
kv_addr_t ku_saved_sp,ku_kernel_cr3;
extern void ku_mode_enter(kv_addr_t entry,kv_addr_t stack,kv_addr_t root);
extern void ku_mode_resume(const registers_t *frame,kv_addr_t root);
extern void ku_mode_leave(void) __attribute__((noreturn));
static ku_space process;
static ks_service services;
static u32 service_generation;
static int rendered_ok,app_resident,app_slices;
static u32 app_total_yields;
static char app_error[96];
static struct {u32 type,delta; i32 x,y,key,button;} app_events[8];
static unsigned app_event_count;
static u8 entry_stack[8192] __attribute__((aligned(4096)));
static u8 scratch[4096],program[256],observed[32];
static volatile int active,busy,reason,vector,copy_ok;
static volatile u32 exit_value,fault_error;
static volatile kv_addr_t fault_address;
static u64 deadline,slice_deadline;
static registers_t continuation;
static volatile u32 peer_beats;
static volatile int peer_running;
static u32 last_yields,last_peer_steps;
static kv_addr_t old_tss;
enum { ENDED=1,FAULTED,TIMED_OUT,YIELDED,KILLED,WAITING };
static void peer_entry(void) {while(peer_running) {peer_beats++;task_yield();}}
static u32 page_alloc(void *ctx) {
    (void)ctx;u32 p=pmm_alloc_frame();
    if(p>=KU_DIRECT_LIMIT || (p>=KU_BASE && p<KU_BASE+KU_SIZE)) {if(p)pmm_free_frame(p);return 0;}
    return p;
}
static void page_free(void *ctx,u32 p) {(void)ctx;pmm_free_frame(p);}
static void *page_direct(void *ctx,u32 p) {(void)ctx;return (void *)(kv_addr_t)p;}
static const ku_ops memory_ops={page_alloc,page_free,page_direct,NULL};
static kv_addr_t root_now(void) {kv_addr_t r;__asm__ volatile("mov %%cr3,%0":"=r"(r));return r;}
static int nx_enable(void) {
#ifdef __x86_64__
    u32 a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000u),"c"(0));
    if(a<0x80000001u)return 0;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001u),"c"(0));
    if(!(d&(1u<<20)))return 0;
    __asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(0xc0000080u));
    a|=1u<<11;__asm__ volatile("wrmsr"::"a"(a),"d"(d),"c"(0xc0000080u):"memory");
    return 1;
#else
    return 0;
#endif
}
static kv_addr_t prior_cr0,prior_cr4;
static u64 prior_sysenter;
#ifdef __x86_64__
static u64 prior_efer;
#endif
static int policy_active,has_sep;
static u64 msr_read(u32 id) {u32 a,d;__asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(id));return ((u64)d<<32)|a;}
static void msr_write(u32 id,u64 value) {__asm__ volatile("wrmsr"::"c"(id),"a"((u32)value),"d"((u32)(value>>32)):"memory");}
static void policy_enter(void) {
    __asm__ volatile("mov %%cr4,%0":"=r"(prior_cr4));
    kv_addr_t cr4=prior_cr4&~((kv_addr_t)(1u<<8)|(kv_addr_t)(1u<<16)|(kv_addr_t)(1u<<18));
    __asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory"); /* no user RDPMC/FSGSBASE/XSAVE family */
    __asm__ volatile("mov %%cr0,%0":"=r"(prior_cr0));
    kv_addr_t bits=prior_cr0|10u; /* TS+MP: no FPU/SIMD without context support */
    __asm__ volatile("mov %0,%%cr0"::"r"(bits):"memory");
    if(has_sep) {prior_sysenter=msr_read(0x174);msr_write(0x174,0);}
#ifdef __x86_64__
    prior_efer=msr_read(0xc0000080);msr_write(0xc0000080,prior_efer&~1ull);
#endif
    policy_active=1;
}
static void policy_leave(void) {
    if(!policy_active)return;
    __asm__ volatile("mov %0,%%cr0"::"r"(prior_cr0):"memory");
    if(has_sep)msr_write(0x174,prior_sysenter);
#ifdef __x86_64__
    msr_write(0xc0000080,prior_efer);
#endif
    __asm__ volatile("mov %0,%%cr4"::"r"(prior_cr4):"memory");
    policy_active=0;
}
static void finish(int why) __attribute__((noreturn));
static void finish(int why) {
    reason=why;active=0;policy_leave();set_kernel_stack(old_tss);ku_mode_leave();
}
int ku_mode_trap(registers_t *r) {
    if(!active || (r->cs&3)!=3)return 0;
    if(r->int_no<32) {
        vector=(int)r->int_no;fault_error=(u32)r->err_code;
        if(vector==14)fault_address=read_cr2();
        finish(FAULTED);
    }
    if(r->int_no!=128)return 0;
#ifdef __x86_64__
    u64 id=r->rax,off=r->rbx,len=r->rcx;int result=-1;
#else
    u32 id=r->eax,off=r->ebx,len=r->ecx;int result=-1;
#endif
    int service_wait=0;
    if(id<=0xffffffffu && off<=0xffffffffu && len<=0xffffffffu) {
        if(id==0 && !len) {exit_value=(u32)off;finish(ENDED);}
        if(id==1) {
            u32 request[5]; /* snapshot once: version,size,src offset,dst offset,length */
            if(len==sizeof(request) && !ku_copyin(&process,request,(u32)off,sizeof(request))) {
                if(request[0]==1 && request[1]==sizeof(request) && request[4]<=sizeof(scratch)) {
                    result=-2;
                    if(!ku_check(&process,request[3],request[4],KU_R|KU_W) &&
                       !ku_copyin(&process,scratch,request[2],request[4]) &&
                       !ku_copyout(&process,request[3],scratch,request[4]))result=0;
                }
            } else result=-2;
        } else if(id==2) {
            int pending=services.pending;result=ks_dispatch(&services,(u32)off,(u32)len);
            service_wait=(pending && !services.pending && result==0) || result==KS_AGAIN;
        }
        else if(id==3 && !off && !len)result=(int)(u32)timer_ticks();
        else if(id!=0)result=-3;
    }
#ifdef __x86_64__
    r->rax=(u64)(int64_t)result;
#else
    r->eax=(u32)result;
#endif
    if(app_resident && service_wait) {memcpy(&continuation,r,sizeof(continuation));finish(WAITING);}
    return 1;
}
int ku_mode_tick(registers_t *r) {
    if(!active || (r->cs&3)!=3)return 0;
    if(timer_ticks()>=deadline)finish(TIMED_OUT);
    if(timer_ticks()>=slice_deadline) {memcpy(&continuation,r,sizeof(continuation));finish(YIELDED);}
    return 1;
}
static int ncode,code_bad;
static void emit(u8 c) {if(ncode<(int)sizeof(program))program[ncode++]=c;else code_bad=1;}
static void word(u32 x) {for(int i=0;i<4;i++)emit((u8)(x>>(8*i)));}
static void mov_a(u32 a) {emit(0xb8);word(a);}
static void write_at(u32 a) {mov_a(a);emit(0xc7);emit(0);word(0x12345678);}
static void exit_code(u32 n) {mov_a(0);emit(0xbb);word(n);emit(0x31);emit(0xc9);emit(0xcd);emit(0x80);}
static void service_call(u32 off) {mov_a(2);emit(0xbb);word(off);emit(0xb9);word(sizeof(ks_request));emit(0xcd);emit(0x80);}
static void store_result(u32 where) {emit(0xba);word(KU_BASE+where);emit(0x89);emit(2);}
static void exit_result(void) {emit(0x89);emit(0xc3);mov_a(0);emit(0x31);emit(0xc9);emit(0xcd);emit(0x80);}
static void render_probe(void *ctx,const ks_rect *r,unsigned n) {
    (void)ctx;rendered_ok=n==1 && r[0].x==0 && r[0].y==0 && r[0].w==40 && r[0].h==40;
}
static int service_setup(int kind) {
    if(!service_generation || ks_init(&services,&process,service_generation,100,80,render_probe,NULL))return 0;
    ks_request r={KS_VERSION,sizeof(r),KS_WAIT,0,service_generation,0,8192,sizeof(ks_event),0,0};
    if(kind==20 || kind==21) {
        if(ks_post(&services,KS_EVENT_DRAW,999,0,0,0,0) || ku_load(&process,4096,&r,sizeof(r)))return 0;
        if(kind==21) {
            r.op=KS_DRAW;r.token=services.event.id;r.a=4096+256;r.b=1;
            ks_rect rect={1,0x123456,-10,-20,50,60};
            if(ku_load(&process,4096+64,&r,sizeof(r)) || ku_load(&process,4096+256,&rect,sizeof(rect)))return 0;
        }
    } else {
        r.op=KS_OPEN;r.a=4096+256;r.b=9;r.c=KS_OPEN_R|KS_OPEN_W|KS_OPEN_CREATE;
        if(kind==23)r.generation--;
        if(kind==25)r.flags=1;
        if(ku_load(&process,4096,&r,sizeof(r)) || ku_load(&process,4096+256,"test.txt",9))return 0;
        r.generation=service_generation;r.flags=0;r.op=kind==24?KS_CLOSE:KS_WRITE;r.a=0;
        r.b=kind==24?0:4096+320;r.c=kind==24?0:32;
        if(ku_load(&process,4096+64,&r,sizeof(r)))return 0;
        r.op=KS_READ;r.b=8192+64;r.c=32;
        if(ku_load(&process,4096+128,&r,sizeof(r)) || ku_load(&process,4096+320,scratch,32))return 0;
    }
    return 1;
}
static int resume_valid(void) {
#ifdef __x86_64__
    kv_addr_t ip=continuation.rip,sp=continuation.rsp;
    continuation.rflags=(continuation.rflags&0xcd5u)|0x202u;
#else
    kv_addr_t ip=continuation.eip,sp=continuation.useresp;
    continuation.eflags=(continuation.eflags&0xcd5u)|0x202u;continuation.ds=0x23;
#endif
    return continuation.cs==0x1b && continuation.ss==0x23 && ip>=KU_BASE && ip<KU_BASE+KU_SIZE &&
           sp>KU_BASE && sp<=KU_BASE+KU_SIZE && !ku_check(&process,(u32)(ip-KU_BASE),1,KU_R|KU_X) &&
           !ku_check(&process,(u32)(sp-1-KU_BASE),1,KU_R|KU_W);
}
static int execute(int reqkind,int nx) {
    if(code_bad || !ncode)return -1;
    last_yields=last_peer_steps=0;u32 peer_start=peer_beats;
    ku_kernel_cr3=root_now();reason=vector=copy_ok=0;exit_value=fault_error=0;fault_address=0;
    kv_flags_t flags=irq_save();
    int ok=ku_create(&process,&memory_ops,(u32)ku_kernel_cr3,sizeof(void *)==8,nx)==0;
    if(ok)ok=!ku_map(&process,0,KU_R|KU_X) && !ku_map(&process,4096,KU_R|KU_W) &&
             !ku_map(&process,8192,KU_R|KU_W) && !ku_map(&process,KU_SIZE-4096,KU_R|KU_W);
    u32 request[5]={1,20,4096+64,8192+64,32};
    if(reqkind==2)request[2]=KU_SIZE-16; /* crosses the arena, no partial copy */
    if(reqkind==3)request[3]=0;         /* RX destination */
    if(reqkind==8)request[4]=4097;
    if(reqkind==9) {request[2]=KU_SIZE;request[3]=KU_SIZE;request[4]=0;}
    if(reqkind==4)request[0]=999;       /* unsupported wire version */
    for(unsigned i=0;i<32;i++)scratch[i]=(u8)(i+17);
    if(ok)ok=!ku_load(&process,0,program,(u32)ncode) &&
             !ku_load(&process,4096,request,sizeof(request)) && !ku_load(&process,4096+64,scratch,32);
    rendered_ok=0;
    if(ok && reqkind>=20 && reqkind<=25) {service_generation++;ok=service_setup(reqkind);}
    if(!ok) {ks_destroy(&services);ku_destroy(&process);irq_restore(flags);return -1;}
    ku_pin(&process);old_tss=get_kernel_stack();set_kernel_stack((kv_addr_t)(entry_stack+sizeof(entry_stack)));
    deadline=timer_ticks()+timer_hz()/10+2;slice_deadline=timer_ticks()+2;policy_enter();active=1;
    /* Saved kernel flags deliberately have IF=0: restoration/teardown is atomic. */
    ku_mode_enter(KU_BASE,KU_BASE+KU_SIZE-16,(kv_addr_t)process.root);
    while(reason==YIELDED) {
        last_yields++;
        if(reqkind==30 && last_yields==3) {reason=KILLED;break;}
        irq_restore(flags);task_yield();flags=irq_save();
        if(!resume_valid()) {reason=FAULTED;vector=13;break;}
        old_tss=get_kernel_stack();set_kernel_stack((kv_addr_t)(entry_stack+sizeof(entry_stack)));
        slice_deadline=timer_ticks()+2;policy_enter();active=1;ku_mode_resume(&continuation,(kv_addr_t)process.root);
    }
    last_peer_steps=peer_beats-peer_start;
    ku_unpin(&process);
    if(reqkind && reqkind<20) {
        ku_copyin(&process,observed,8192+64,32);copy_ok=1;
        for(unsigned i=0;i<32;i++)if(observed[i]!=(reqkind==1?(u8)(i+17):0))copy_ok=0;
    }
    if(reqkind==40) {
        copy_ok=!ku_copyin(&process,scratch,8192,1024);
        for(unsigned i=0;i<1024;i++)if(scratch[i])copy_ok=0;
    }
    if(reqkind==20) {
        ks_event e;copy_ok=!ku_copyin(&process,&e,8192,sizeof(e)) && e.generation==service_generation && e.delta_ms==250 && e.type==KS_EVENT_DRAW;
    } else if(reqkind==21)copy_ok=rendered_ok;
    else if(reqkind==22) {
        copy_ok=!ku_copyin(&process,observed,8192+64,32);
        for(unsigned i=0;i<32;i++)if(observed[i]!=(u8)(i+17))copy_ok=0;
    } else if(reqkind>=23 && reqkind<=25) {
        copy_ok=1;
        for(unsigned i=0;i<KS_HANDLES;i++)if(services.handles[i].cookie)copy_ok=0;
    }
    ks_destroy(&services);memset(&continuation,0,sizeof(continuation));
    int status=ku_destroy(&process);irq_restore(flags);
    return status;
}
static int report(const char *name,int expected,int fault,u32 value,u32 frames,int run) {
    int vec_ok=vector==fault || (fault==-1 && (vector==6 || vector==13));
#ifdef __x86_64__
    /* Jumping to a noncanonical address raises #GP(0) on hardware, but QEMU
       TCG may instead deliver an instruction-fetch #PF (U+I/D set, CR2 equal
       to the jump target). Both prove the CPL3 fault stayed contained, which
       is the property this fixture checks; accept either delivery. */
    if(!strcmp(name,"noncanonical") && vector==14 && (fault_error&20)==20 &&
       fault_address==(kv_addr_t)0x0000800000000000ULL) vec_ok=1;
#endif
    int ok=!run && reason==expected && (expected!=FAULTED || vec_ok) &&
           (expected!=ENDED || exit_value==value) && pmm_used_frames()==frames &&
           root_now()==ku_kernel_cr3 && !process.live && get_kernel_stack()==old_tss;
    kv_addr_t cr0,cr4;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));__asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    if(cr0!=prior_cr0 || cr4!=prior_cr4 || (has_sep && msr_read(0x174)!=prior_sysenter))ok=0;
#ifdef __x86_64__
    if(msr_read(0xc0000080)!=prior_efer)ok=0;
#endif
    if(expected==FAULTED && fault==14) {
        if(!(fault_error&4))ok=0;
        kv_addr_t want=!strcmp(name,"kernel-write") || !strcmp(name,"kernel-read")?0x100000:
                       !strcmp(name,"null-write")?0:!strcmp(name,"guard-write")?KU_BASE+KU_SIZE-8192:
                       !strcmp(name,"RX-write")?KU_BASE:!strcmp(name,"NX-data")?KU_BASE+4096:fault_address;
        if(fault_address!=want)ok=0;
        if((!strcmp(name,"kernel-write") || !strcmp(name,"RX-write")) && fault_error!=7)ok=0;
        if(!strcmp(name,"kernel-read") && fault_error!=5)ok=0;
        if(!strcmp(name,"NX-data") && fault_error!=21)ok=0;
        if(!strcmp(name,"stack-overflow") && (fault_address<KU_BASE+KU_SIZE-8192 || fault_address>=KU_BASE+KU_SIZE-4096))ok=0;
    }
    if(!strcmp(name,"infinite-loop") && (!last_yields || !last_peer_steps))ok=0;
    if(!strcmp(name,"resume-registers") && (last_yields<4 || !last_peer_steps))ok=0;
    if(!strcmp(name,"kill-suspended") && last_yields!=3)ok=0;
    kprintf("[userlab] %s %s reason=%d vector=%d error=%u value=%u frames=%u slices=%u peer=%u\n",ok?"PASS":"FAIL",name,reason,vector,fault_error,exit_value,pmm_used_frames(),last_yields,last_peer_steps);
    return !ok;
}
void ku_selftest(void) {
    if(busy || app_resident || kapp_in_app()) {kputs("[userlab] busy\n");return;}
    busy=1;peer_running=1;peer_beats=0;
    if(!task_create("userlab-peer",peer_entry)) {peer_running=busy=0;kputs("[userlab] FAIL peer OOM\n");return;}
    u32 ca,cb,cc,cd;__asm__ volatile("cpuid":"=a"(ca),"=b"(cb),"=c"(cc),"=d"(cd):"a"(1),"c"(0));has_sep=!!(cd&(1u<<11));
    int failures=0,nx=nx_enable();u32 frames=pmm_used_frames();
    u32 kernel_word=*(volatile u32 *)(kv_addr_t)0x100000;
    ncode=0;code_bad=0;exit_code(42);failures+=report("exit",ENDED,0,42,frames,execute(0,nx));
    const char *names[]={"kernel-write","null-write","guard-write","RX-write","CLI","OUT","HLT","UD2","divide-zero","kernel-read","infinite-loop","stack-overflow","read-CR3","FPU-disabled"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        ncode=0;code_bad=0;int expected=FAULTED,fault=14;
        if(i<4)write_at(i==0?0x100000:i==1?0:i==2?KU_BASE+KU_SIZE-8192:KU_BASE);
        else if(i==4) {emit(0xfa);fault=13;}
        else if(i==5) {emit(0x31);emit(0xc0);emit(0xe6);emit(0x80);fault=13;}
        else if(i==6) {emit(0xf4);fault=13;}
        else if(i==7) {emit(0x0f);emit(0x0b);fault=6;}
        else if(i==8) {emit(0x31);emit(0xd2);emit(0x31);emit(0xc9);mov_a(1);emit(0xf7);emit(0xf1);fault=0;}
        else if(i==9) {mov_a(0x100000);emit(0x8b);emit(0);}
        else if(i==10) {emit(0xeb);emit(0xfe);expected=TIMED_OUT;}
        else if(i==11) {emit(0xbc);word(KU_BASE+KU_SIZE-4096+8);emit(0x50);emit(0x50);emit(0x50);}
        else if(i==12) {emit(0x0f);emit(0x20);emit(0xc0);fault=13;}
        else {emit(0xd9);emit(0xe8);fault=7;}
        exit_code(99);failures+=report(names[i],expected,fault,0,frames,execute(0,nx));
        if(*(volatile u32 *)(kv_addr_t)0x100000!=kernel_word)failures++;
    }
    /* Probe state-save/restore families, not just FLD1. RW+aligned operand
       avoids confusing #NM/#UD policy with a guard-page or alignment fault. */
    const struct {const char *name;u8 bytes[3];int count,feature;} fp_denied[]={
        {"FWAIT-disabled",{0x9b},1,0},{"FNINIT-disabled",{0xdb,0xe3},2,0},
        {"FNSTENV-disabled",{0xd9,0x33},2,0},{"FNSAVE-disabled",{0xdd,0x33},2,0},
        {"MMX-disabled",{0x0f,0xef,0xc0},3,1},{"SSE-disabled",{0x0f,0x57,0xc0},3,2},
        {"STMXCSR-disabled",{0x0f,0xae,0x1b},3,2},{"LDMXCSR-disabled",{0x0f,0xae,0x13},3,2},
        {"FXSAVE-disabled",{0x0f,0xae,0x03},3,3},{"FXRSTOR-disabled",{0x0f,0xae,0x0b},3,3},
        {"XSAVE-disabled",{0x0f,0xae,0x23},3,4},{"XRSTOR-disabled",{0x0f,0xae,0x2b},3,4}
    };
    kv_addr_t host_cr0,host_cr4;
    __asm__ volatile("mov %%cr0,%0":"=r"(host_cr0));__asm__ volatile("mov %%cr4,%0":"=r"(host_cr4));
    for(unsigned i=0;i<sizeof(fp_denied)/sizeof(fp_denied[0]);i++) {
        int feature=fp_denied[i].feature,fault=7;
        if((feature==1 && !(cd&(1u<<23))) ||
           (feature==2 && (!(cd&(1u<<25)) || !(host_cr4&(1u<<9)) || (host_cr0&4))) ||
           (feature==3 && !(cd&(1u<<24))) ||
           (feature==4 && (!(cc&(1u<<26)) || !(host_cr4&(1u<<18)))))fault=6;
        ncode=0;code_bad=0;emit(0xbb);word(KU_BASE+8192);mov_a(3);emit(0x31);emit(0xd2);
        for(int j=0;j<fp_denied[i].count;j++)emit(fp_denied[i].bytes[j]);
        exit_code(99);failures+=report(fp_denied[i].name,FAULTED,fault,0,frames,execute(40,nx));
        if(!copy_ok) {kputs("[userlab] FAIL FP state leaked/partially stored\n");failures++;}
    }
    if(cc&(1u<<26)) {
        /* Refuse inherited OSXSAVE explicitly: do not rely only on TS checks
           in every CPU/emulator implementation of XSAVE/XRSTOR. */
        kv_addr_t enabled=host_cr4|(1u<<18);
        __asm__ volatile("mov %0,%%cr4"::"r"(enabled):"memory");
        for(unsigned i=0;i<2;i++) {
            ncode=0;code_bad=0;emit(0xbb);word(KU_BASE+8192);mov_a(3);emit(0x31);emit(0xd2);
            emit(0x0f);emit(0xae);emit(i?0x2b:0x23);exit_code(99);
            failures+=report(i?"XRSTOR-inherited-denied":"XSAVE-inherited-denied",FAULTED,6,0,frames,execute(40,nx));
            if(!copy_ok) {kputs("[userlab] FAIL inherited XSAVE state wrote user memory\n");failures++;}
        }
        __asm__ volatile("mov %0,%%cr4"::"r"(host_cr4):"memory");
    }
    const char *copies[]={"copy-good","copy-wrap","copy-RX","copy-version","copy-unknown","copy-header-size","copy-header-wrap","copy-quota","copy-empty"};
    for(int kind=1;kind<=9;kind++) {
        ncode=0;code_bad=0;mov_a(kind==5?777:1);emit(0xbb);word(kind==7?KU_SIZE-16:4096);emit(0xb9);word(kind==6?19:20);emit(0xcd);emit(0x80);
        emit(0x89);emit(0xc3);mov_a(0);emit(0x31);emit(0xc9);emit(0xcd);emit(0x80);
        failures+=report(copies[kind-1],ENDED,0,kind==1 || kind==9?0:kind==4 || kind==8?0xffffffffu:kind==5?0xfffffffdu:0xfffffffeu,frames,execute(kind,nx));
        if(!copy_ok) {kputs("[userlab] FAIL partial/missing copy\n");failures++;}
    }
#ifdef __x86_64__
    if(nx) {
        ncode=0;code_bad=0;mov_a(KU_BASE+4096);emit(0xff);emit(0xe0);
        failures+=report("NX-data",FAULTED,14,0,frames,execute(0,nx));
    }
    ncode=0;code_bad=0;emit(0x48);emit(0xb8);word(0);word(0x00008000);emit(0xff);emit(0xe0);
    failures+=report("noncanonical",FAULTED,13,0,frames,execute(0,nx));
    ncode=0;code_bad=0;mov_a(1);emit(0x48);emit(0xbb);word(4096);word(1);emit(0xb9);word(20);emit(0xcd);emit(0x80);
    emit(0x89);emit(0xc3);mov_a(0);emit(0x31);emit(0xc9);emit(0xcd);emit(0x80);
    failures+=report("syscall-upper-half",ENDED,0,0xffffffffu,frames,execute(4,nx));
#endif
    if(has_sep) {
        ncode=0;code_bad=0;emit(0x0f);emit(0x34);
        failures+=report("sysenter-disabled",FAULTED,-1,0,frames,execute(0,nx));
    }
#ifdef __x86_64__
    ncode=0;code_bad=0;emit(0x0f);emit(0x05);
    failures+=report("syscall-disabled",FAULTED,6,0,frames,execute(0,nx));
    __asm__ volatile("cpuid":"=a"(ca),"=b"(cb),"=c"(cc),"=d"(cd):"a"(7),"c"(0));
    if(cb&1u) {
        kv_addr_t old_cr4;__asm__ volatile("mov %%cr4,%0":"=r"(old_cr4));
        kv_addr_t enabled=old_cr4|(1u<<16)|(1u<<8);
        __asm__ volatile("mov %0,%%cr4"::"r"(enabled):"memory");
        ncode=0;code_bad=0;mov_a(0x12345678);emit(0xf3);emit(0x0f);emit(0xae);emit(0xd0);
        failures+=report("fsgsbase-disabled",FAULTED,6,0,frames,execute(0,nx));
        __asm__ volatile("mov %0,%%cr4"::"r"(old_cr4):"memory");
    }
#endif
    const char *service_names[]={"event-wire","draw-clipped","RAM-capabilities","service-generation","stale-handle","service-flags"};
    for(int kind=20;kind<=25;kind++) {
        ncode=0;code_bad=0;service_call(4096);
        if(kind==21)service_call(4096+64);
        if(kind==22 || kind==24) {
            store_result(4096+64+24);
            if(kind==22)store_result(4096+128+24);
            service_call(4096+64);service_call(4096+(kind==24?64:128));
        }
        exit_result();
        u32 expected=kind==20?sizeof(ks_event):kind==21?1:kind==22?32:kind==23?(u32)KS_DENIED:kind==24?(u32)KS_STALE:(u32)KS_INVALID;
        failures+=report(service_names[kind-20],ENDED,0,expected,frames,execute(kind,nx));
        if(!copy_ok) {kputs("[userlab] FAIL service side effect\n");failures++;}
    }
    ncode=0;code_bad=0;emit(0xeb);emit(0xfe);
    failures+=report("kill-suspended",KILLED,0,0,frames,execute(30,nx));
    ncode=0;code_bad=0;emit(0xfd);emit(0x68);word(0x55553333);
#ifdef __x86_64__
    emit(0x49);emit(0xbc);word(0x76543210);word(0xfedcba98);
#endif
    mov_a(3);emit(0x31);emit(0xdb);emit(0x31);emit(0xc9);emit(0xcd);emit(0x80);
    emit(0x89);emit(0xc7);emit(0xbd);word(0x12345678);
    int loop=ncode;mov_a(3);emit(0x31);emit(0xdb);emit(0x31);emit(0xc9);emit(0xcd);emit(0x80);
    emit(0x29);emit(0xf8);emit(0x83);emit(0xf8);emit(12);emit(0x72);emit((u8)(loop-(ncode+1)));
    emit(0x81);emit(0xfd);word(0x12345678);emit(0x75);int bad1=ncode;emit(0);
    emit(0x5a);emit(0x81);emit(0xfa);word(0x55553333);emit(0x75);int bad2=ncode;emit(0);
    emit(0x9c);emit(0x58);emit(0xa9);word(0x400);emit(0x74);int bad3=ncode;emit(0);
#ifdef __x86_64__
    emit(0x48);emit(0xba);word(0x76543210);word(0xfedcba98);emit(0x49);emit(0x39);emit(0xd4);emit(0x75);int bad4=ncode;emit(0);
#endif
    exit_code(42);program[bad3]=(u8)(ncode-(bad3+1));
#ifdef __x86_64__
    program[bad4]=(u8)(ncode-(bad4+1));
#endif
    program[bad1]=(u8)(ncode-(bad1+1));program[bad2]=(u8)(ncode-(bad2+1));exit_code(99);
    failures+=report("resume-registers",ENDED,0,42,frames,execute(31,nx));
    peer_running=0;task_yield();task_yield();
    kprintf("[userlab] %s failures=%d private CPL3 fixtures; opt-in ABI4 uses the separate strict image loader\n",failures?"FAIL":"ALL PASS",failures);
    busy=0;
}

/* Persistent opt-in format2 runtime. One app; kernel-owned event dispatch, no
   direct calls into user callbacks. Each pump returns at event boundary/quantum. */
int ku_app_live(void) {return app_resident;}
u32 ku_app_draw_token(void) {return app_resident && services.pending && services.event.type==KS_EVENT_DRAW?services.event.id:0;}
const char *ku_app_error(void) {return app_error;}
const char *ku_app_status(void) {return app_resident?services.status:"";}
void ku_app_unload(void) {
    kv_flags_t f=irq_save();
    if(!app_resident || active || busy) {irq_restore(f);return;}
    ks_destroy(&services);ku_unpin(&process);ku_destroy(&process);
    memset(&continuation,0,sizeof(continuation));memset(app_events,0,sizeof(app_events));
    app_event_count=0;app_resident=0;irq_restore(f);
}
int ku_app_load(const void *image,u32 bytes,ks_sink sink,ks_text_sink text_sink,ks_line_sink line_sink,void *ctx) {
    kv_flags_t flags=irq_save();
    if(busy || active || app_resident || service_generation==0xffffffffu) {irq_restore(flags);return -1;}
    app_error[0]=0;busy=1;ki_plan plan;memset(&plan,0,sizeof(plan));ku_kernel_cr3=root_now();
    u32 ca,cb,cc,cd;__asm__ volatile("cpuid":"=a"(ca),"=b"(cb),"=c"(cc),"=d"(cd):"a"(1),"c"(0));has_sep=!!(cd&(1u<<11));
    int nx=nx_enable(),ok=!ki_load(&process,&memory_ops,(u32)ku_kernel_cr3,image,bytes,sizeof(void*)==8,nx,&plan);
    u32 info[8]={1,32,++service_generation,420,300,sizeof(void*)==8,(u32)nx,0};
    if(ok)ok=!ku_load(&process,KI_BOOT_OFF,info,sizeof(info)) && !ks_init(&services,&process,service_generation,420,300,sink,ctx);
    if(ok) {services.text_sink=text_sink;services.line_sink=line_sink;}
    memset(&continuation,0,sizeof(continuation));
#ifdef __x86_64__
    continuation.rip=plan.entry;continuation.rsp=KU_BASE+KU_SIZE-8;continuation.rdi=KI_BOOT_VA;continuation.rflags=0x202;
    u64 initial_stack=0;
    if(ok)ok=!ku_load(&process,KU_SIZE-8,&initial_stack,sizeof(initial_stack));
#else
    continuation.eip=plan.entry;continuation.useresp=KU_BASE+KU_SIZE-20;continuation.ds=0x23;continuation.eflags=0x202;
    u32 initial_stack[5]={0,KI_BOOT_VA,0,0,0};
    if(ok)ok=!ku_load(&process,KU_SIZE-20,initial_stack,sizeof(initial_stack));
#endif
    continuation.cs=0x1b;continuation.ss=0x23;
    if(ok) {ku_pin(&process);app_resident=1;app_event_count=0;app_slices=0;app_total_yields=0;}
    else {ks_destroy(&services);ku_destroy(&process);strcpy(app_error,"invalid isolated image or memory limit");}
    busy=0;irq_restore(flags);return ok?0:-1;
}
int ku_app_event(u32 type,u32 delta,u32 width,u32 height,i32 x,i32 y,i32 key,i32 button) {
    kv_flags_t flags=irq_save();
    if(!app_resident || busy) {irq_restore(flags);return -1;}
    if(!width || !height || width>4096 || height>4096 || type<1 || type>5) {strcpy(app_error,"invalid isolated canvas/event");ku_app_unload();irq_restore(flags);return -1;}
    busy=1;services.width=width;services.height=height;services.hz=timer_hz();services.language=kv_lang_get();
    services.clock_valid=!rtc_cached_clock(services.clock);
    unsigned at=app_event_count;
    if(type==KS_EVENT_DRAW || type==KS_EVENT_TICK)
        for(unsigned i=0;i<app_event_count;i++)if(app_events[i].type==type) {at=i;break;}
    if(at==8) {busy=0;strcpy(app_error,"isolated input queue limit");ku_app_unload();irq_restore(flags);return -1;}
    if(at==app_event_count)app_event_count++;
    u32 elapsed=delta>250?250:delta;
    if(type==KS_EVENT_TICK && app_events[at].type==type) {elapsed+=app_events[at].delta;if(elapsed>250)elapsed=250;}
    app_events[at].type=type;app_events[at].delta=elapsed;
    app_events[at].x=x;app_events[at].y=y;app_events[at].key=key;app_events[at].button=button;
    if(!services.pending) {
        if(ks_post(&services,app_events[0].type,app_events[0].delta,app_events[0].x,app_events[0].y,app_events[0].key,app_events[0].button)) {
            busy=0;strcpy(app_error,"isolated event token limit");ku_app_unload();irq_restore(flags);return -1;
        }
        app_event_count--;memmove(app_events,app_events+1,app_event_count*sizeof(app_events[0]));memset(app_events+app_event_count,0,sizeof(app_events[0]));
    }
    ku_kernel_cr3=root_now();old_tss=get_kernel_stack();
    int valid=!ku_refresh(&process,(u32)ku_kernel_cr3) && resume_valid();reason=vector=0;fault_error=0;fault_address=0;
    if(valid) {
        set_kernel_stack((kv_addr_t)(entry_stack+sizeof(entry_stack)));deadline=timer_ticks()+timer_hz()/10+2;
        slice_deadline=timer_ticks()+2;policy_enter();active=1;ku_mode_resume(&continuation,process.root);
    } else {reason=FAULTED;vector=13;}
    busy=0;
    int result=1;
    if(reason==YIELDED) {app_total_yields++;if(++app_slices>=25)reason=KILLED;}
    if(reason==WAITING)app_slices=0;
    else if(reason!=YIELDED) {
        if(reason==ENDED && !exit_value)result=0;
        else {ksnprintf(app_error,sizeof(app_error),"isolated stop: reason %d, vector %d, value %u",reason,vector,reason==ENDED?exit_value:0);result=-1;}
        ku_app_unload();
    }
    irq_restore(flags);return result;
}
