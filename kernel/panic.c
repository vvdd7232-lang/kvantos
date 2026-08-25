/* Fatal kernel error */
#include "kernel.h"

void panic(const char *msg, registers_t *r) {
    /* If the exception happened inside an application the system must
       not die: unload the application and return to the shell. */
    if (kapp_in_app()) kapp_recover(msg);
    cli();
    vga_panic_screen();
    kprintf("\n");
    kprintf("   ##  KvantOS - KERNEL PANIC  ##\n\n");
    kprintf(T("   Reason: %s\n", "   Причина: %s\n"), msg);
    if (r) {
        kprintf("   int=%u  errcode=0x%08x\n", (u32)r->int_no, (u32)r->err_code);
#ifdef __x86_64__
        kprintf("   rip=0x%08x%08x  rflags=0x%08x\n", (u32)(r->rip >> 32), (u32)r->rip, (u32)r->rflags);
        kprintf("   rax=0x%08x  rbx=0x%08x  rcx=0x%08x  rdx=0x%08x\n", (u32)r->rax, (u32)r->rbx, (u32)r->rcx, (u32)r->rdx);
        kprintf("   rsi=0x%08x  rdi=0x%08x  rbp=0x%08x  rsp=0x%08x\n", (u32)r->rsi, (u32)r->rdi, (u32)r->rbp, (u32)r->rsp);
#else
        kprintf("   eip=0x%08x  cs=0x%04x  eflags=0x%08x\n", r->eip, r->cs, r->eflags);
        kprintf("   eax=0x%08x  ebx=0x%08x  ecx=0x%08x  edx=0x%08x\n", r->eax, r->ebx, r->ecx, r->edx);
        kprintf("   esi=0x%08x  edi=0x%08x  ebp=0x%08x  esp=0x%08x\n", r->esi, r->edi, r->ebp, r->esp_dummy);
#endif
    }
    task_t *t = task_current();
    if (t) kprintf(T("   task: #%u %s\n", "   задача: #%u %s\n"), t->id, t->name);
    kprintf(T("\n   System halted. Press Ctrl+Alt+Del or reset the machine.\n", "\n   Система остановлена. Нажмите Ctrl+Alt+Del или перезагрузите машину.\n"));
    for (;;) { cli(); hlt(); }
}
