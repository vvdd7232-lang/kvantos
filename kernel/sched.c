/* A cooperative/pre-emptive round-robin scheduler */
#include "kernel.h"

#define STACK_SIZE 8192

static task_t *current = NULL;
static task_t *tasks   = NULL;
static u32 next_id = 0;
static int sched_ready = 0;

task_t *task_current(void) { return current; }
task_t *task_list(void)    { return tasks; }

/* Host tests exercise real traversal/copy logic without privileged CLI. */
#ifdef KV_SCHED_HOSTTEST
static kv_flags_t snapshot_lock(void) { return 0; }
static void snapshot_unlock(kv_flags_t flags) { (void)flags; }
#else
static kv_flags_t snapshot_lock(void) { return irq_save(); }
static void snapshot_unlock(kv_flags_t flags) { irq_restore(flags); }
#endif

/* Bound visited nodes, not live ones: a ring of dead tasks must terminate.
   Validate the whole ring before reporting a result, and copy while locked
   so the scheduler's reaper cannot free a task between lookup and copy. */
static int snapshot_find(u32 key, int by_id, task_info_t *out, u32 *count) {
    kv_flags_t flags = snapshot_lock();
    task_t *start = current, *t = start;
    task_info_t found = {0};
    u32 live = 0, visited = 0;
    int have = 0, valid = !start;
    if (start) do {
        if (!t || visited++ == 256) break;
        if (t->state != TASK_DEAD) {
            if ((by_id ? t->id == key : live == key) && !have) {
                found.id = t->id;
                memcpy(found.name, t->name, sizeof(found.name));
                found.name[sizeof(found.name)-1] = 0;
                found.state = t->state;
                found.switches = t->switches;
                have = 1;
            }
            live++;
        }
        t = t->next;
        if (t == start) { valid = 1; break; }
    } while (1);
    if (valid) {
        if (count) *count = live;
        if (out && have) *out = found;
    }
    snapshot_unlock(flags);
    return valid ? have : -1;
}

u32 task_count(void) {
    u32 count = 0;
    snapshot_find(0, 0, NULL, &count);
    return count;
}

int task_snapshot_id(u32 id, task_info_t *out) {
    return out ? snapshot_find(id, 1, out, NULL) : -1;
}

int task_snapshot_nth(u32 index, task_info_t *out) {
    return out ? snapshot_find(index, 0, out, NULL) : -1;
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
    t->stack_base = (kv_addr_t)stack;

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
            kv_addr_t sb = n->stack_base;
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
    u64 delay = (u64)(ms / 1000u) * hz
              + ((ms % 1000u) * hz + 999u) / 1000u;
    if (ms && !delay) delay = 1;
    current->wake_tick = timer_ticks() + delay;
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
