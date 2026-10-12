/* A cooperative/pre-emptive round-robin scheduler */
#include "kernel.h"

#define STACK_SIZE 8192

static task_t *current = NULL;
static task_t *tasks   = NULL;
static u32 next_id = 0;
static int sched_ready = 0;

task_t *task_current(void) { return current; }
task_t *task_list(void)    { return tasks; }

/* The list is circular - iterate strictly until back at the start */
u32 task_count(void) {
    if (!current) return 0;
    kv_flags_t fl = irq_save();
    u32 n = 0;
    task_t *t = current;
    do {
        if (t->state != TASK_DEAD) n++;
        t = t->next;
    } while (t != current && n < 256);
    irq_restore(fl);
    return n;
}

void sched_init(void) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    memset(t, 0, sizeof(task_t));
    strncpy(t->name, "kernel", sizeof(t->name));
    t->id = next_id++;
    t->state = TASK_READY;
    t->next = t;
    tasks = current = t;
    sched_ready = 1;
}

task_t *task_create(const char *name, void (*entry)(void)) {
    if (!entry) return NULL;
    if (!current) return NULL;        /* before sched_init() there is no list yet */
    kv_flags_t fl = irq_save();
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) { irq_restore(fl); return NULL; }
    memset(t, 0, sizeof(task_t));
    strncpy(t->name, name, sizeof(t->name));
    t->id = next_id++;
    t->state = TASK_READY;

    u8 *stack = (u8 *)kmalloc(STACK_SIZE);
    if (!stack) { kfree(t); irq_restore(fl); return NULL; }
    t->stack_base = (u32)stack;

    /* The frame must match context_switch exactly: its pop sequence is
       popfd, edi, esi, ebx, ebp, ret. So from the top of the stack
       upwards: eflags, edi, esi, ebx, ebp, entry, task_exit.
       64-bit pops r15,r14,r13,r12,rbx,rbp, then popfq, then ret. */
    kv_addr_t *sp = (kv_addr_t *)(stack + STACK_SIZE);
    *--sp = (kv_addr_t)task_exit;             /* return address out of entry */
    *--sp = (kv_addr_t)entry;                 /* ret will jump here */
#ifdef __x86_64__
    *--sp = 0x00000202;                       /* RFLAGS with IF=1 (popfq) */
    *--sp = 0;                                /* rbp */
    *--sp = 0;                                /* rbx */
    *--sp = 0;                                /* r12 */
    *--sp = 0;                                /* r13 */
    *--sp = 0;                                /* r14 */
    *--sp = 0;                                /* r15 */
#else
    *--sp = 0;                                /* ebp */
    *--sp = 0;                                /* ebx */
    *--sp = 0;                                /* esi */
    *--sp = 0;                                /* edi */
    *--sp = 0x00000202;                       /* EFLAGS with IF=1 (popfd) */
#endif
    t->esp = (kv_addr_t)sp;

    /* insertion into the circular list */
    t->next = current->next;
    current->next = t;
    irq_restore(fl);
    return t;
}

/* Remove one dead task from the ring per call (never the current one) */
static void reap_one(void) {
    task_t *p = current;
    for (int i = 0; i < 64; i++) {
        task_t *n = p->next;
        if (n == current) return;
        if (n->state == TASK_DEAD) {
            p->next = n->next;
            if (tasks == n) tasks = n->next;
            u32 sb = n->stack_base;
            kfree(n);
            if (sb) kfree((void *)sb);
            return;
        }
        p = n;
    }
}

/* Circular search for the next ready task, starting after the current one */
static task_t *pick_next(void) {
    u64 now = timer_ticks();
    task_t *t = current->next;
    for (int i = 0; i < 256; i++) {
        if (t->state == TASK_SLEEPING && now >= t->wake_tick) t->state = TASK_READY;
        if (t->state == TASK_READY) return t;
        if (t == current) break;      /* a full turn */
        t = t->next;
    }
    return (current->state == TASK_READY) ? current : NULL;
}

void schedule(void) {
    if (!sched_ready || !current) return;
    kv_flags_t fl = irq_save();

    reap_one();

    task_t *next = pick_next();
    if (!next || next == current) { irq_restore(fl); return; }

    task_t *prev = current;
    current = next;
    current->switches++;
    context_switch(&prev->esp, next->esp);
    irq_restore(fl);
}

void task_yield(void) { schedule(); }

void task_sleep(u32 ms) {
    if (!sched_ready) { sleep_ms(ms); return; }
    kv_flags_t fl = irq_save();
    current->state = TASK_SLEEPING;
    u32 hz = timer_hz();
    current->wake_tick = timer_ticks() + (u64)(ms / 1000u * hz + (ms % 1000u) * hz / 1000u);
    irq_restore(fl);
    schedule();
}

/* Terminate another task by id. The task is marked dead and is
   collected by the reaper on one of the following schedule() calls,
   exactly like one that finished on its own. The current task cannot
   be killed this way (it would never return from the call): the
   caller gets -2 and can decide whether to exit voluntarily. */
int task_kill(u32 id) {
    if (!sched_ready || !current) return -1;
    kv_flags_t fl = irq_save();
    task_t *t = current;
    int guard = 256;
    int found = -1;
    do {
        if (t->id == id && t->state != TASK_DEAD) {
            if (t == current) { found = -2; break; }
            t->state = TASK_DEAD;
            found = 0;
            break;
        }
        t = t->next;
    } while (t != current && --guard > 0);
    irq_restore(fl);
    return found;
}

void task_exit(void) {
    kv_flags_t fl = irq_save();
    current->state = TASK_DEAD;
    irq_restore(fl);
    for (;;) { schedule(); __asm__ volatile("hlt"); }
}
