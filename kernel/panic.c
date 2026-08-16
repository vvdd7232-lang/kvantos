/* Критическая ошибка ядра */
#include "kernel.h"

void panic(const char *msg, registers_t *r) {
    /* Если исключение произошло внутри приложения, система не должна
       умирать: снимаем приложение и возвращаемся в оболочку. */
    if (kapp_in_app()) kapp_recover(msg);
    cli();
    vga_panic_screen();
    kprintf("\n");
    kprintf("   ##  KvantOS - KERNEL PANIC  ##\n\n");
    kprintf("   Причина: %s\n", msg);
    if (r) {
        kprintf("   int=%u  errcode=0x%08x\n", r->int_no, r->err_code);
        kprintf("   eip=0x%08x  cs=0x%04x  eflags=0x%08x\n", r->eip, r->cs, r->eflags);
        kprintf("   eax=0x%08x  ebx=0x%08x  ecx=0x%08x  edx=0x%08x\n", r->eax, r->ebx, r->ecx, r->edx);
        kprintf("   esi=0x%08x  edi=0x%08x  ebp=0x%08x  esp=0x%08x\n", r->esi, r->edi, r->ebp, r->esp_dummy);
    }
    task_t *t = task_current();
    if (t) kprintf("   задача: #%u %s\n", t->id, t->name);
    kprintf("\n   Система остановлена. Нажмите Ctrl+Alt+Del или перезагрузите машину.\n");
    for (;;) { cli(); hlt(); }
}
