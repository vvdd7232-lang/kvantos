/* Кооперативно-вытесняющий круговой планировщик (round-robin) */
#include "kernel.h"

#define STACK_SIZE 8192

static task_t *current = NULL;
static task_t *tasks   = NULL;
static u32 next_id = 0;
static int sched_ready = 0;

task_t *task_current(void) { return current; }
task_t *task_list(void)    { return tasks; }

/* Список кольцевой - обход строго до возврата в начало */
u32 task_count(void) {
    if (!current) return 0;
    u32 fl = irq_save();
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
    if (!current) return NULL;        /* до sched_init() списка ещё нет */
    u32 fl = irq_save();
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) { irq_restore(fl); return NULL; }
    memset(t, 0, sizeof(task_t));
    strncpy(t->name, name, sizeof(t->name));
    t->id = next_id++;
    t->state = TASK_READY;

    u8 *stack = (u8 *)kmalloc(STACK_SIZE);
    if (!stack) { kfree(t); irq_restore(fl); return NULL; }
    t->stack_base = (u32)stack;

    /* Кадр должен точно соответствовать context_switch:
       pop-последовательность там - popfd, edi, esi, ebx, ebp, ret.
       Значит от вершины стека вверх: eflags, edi, esi, ebx, ebp, entry, task_exit. */
    u32 *sp = (u32 *)(stack + STACK_SIZE);
    *--sp = (u32)task_exit;               /* адрес возврата из entry */
    *--sp = (u32)entry;                   /* сюда прыгнет ret */
    *--sp = 0;                            /* ebp */
    *--sp = 0;                            /* ebx */
    *--sp = 0;                            /* esi */
    *--sp = 0;                            /* edi */
    *--sp = 0x00000202;                   /* EFLAGS с IF=1 (popfd) */
    t->esp = (u32)sp;

    /* вставка в кольцевой список */
    t->next = current->next;
    current->next = t;
    irq_restore(fl);
    return t;
}

/* Снимаем с кольца одну мёртвую задачу за вызов (не текущую) */
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

/* Круговой поиск следующей готовой задачи, начиная со следующей за текущей */
static task_t *pick_next(void) {
    u64 now = timer_ticks();
    task_t *t = current->next;
    for (int i = 0; i < 256; i++) {
        if (t->state == TASK_SLEEPING && now >= t->wake_tick) t->state = TASK_READY;
        if (t->state == TASK_READY) return t;
        if (t == current) break;      /* полный оборот */
        t = t->next;
    }
    return (current->state == TASK_READY) ? current : NULL;
}

void schedule(void) {
    if (!sched_ready || !current) return;
    u32 fl = irq_save();

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
    u32 fl = irq_save();
    current->state = TASK_SLEEPING;
    u32 hz = timer_hz();
    current->wake_tick = timer_ticks() + (u64)(ms / 1000u * hz + (ms % 1000u) * hz / 1000u);
    irq_restore(fl);
    schedule();
}

void task_exit(void) {
    u32 fl = irq_save();
    current->state = TASK_DEAD;
    irq_restore(fl);
    for (;;) { schedule(); __asm__ volatile("hlt"); }
}
