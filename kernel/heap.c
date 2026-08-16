/* Kernel heap: a first-fit block list that coalesces neighbours */
#include "kernel.h"

#define MAGIC_USED 0xC0FFEE01
#define MAGIC_FREE 0xC0FFEE02
#define ALIGN8(x)  (((x) + 7) & ~7u)

typedef struct block {
    u32 magic;
    u32 size;              /* size of the usable area */
    struct block *next;
    struct block *prev;
    int free;
} block_t;

static block_t *head = NULL;
static u32 heap_total = 0, heap_used = 0;

void heap_init(u32 start, u32 size) {
    head = (block_t *)start;
    head->magic = MAGIC_FREE;
    head->size  = size - sizeof(block_t);
    head->next  = NULL;
    head->prev  = NULL;
    head->free  = 1;
    heap_total  = size;
    heap_used   = sizeof(block_t);
}

static void split(block_t *b, u32 size) {
    if (b->size < size + sizeof(block_t) + 16) return;
    block_t *n = (block_t *)((u8 *)b + sizeof(block_t) + size);
    n->magic = MAGIC_FREE;
    n->size  = b->size - size - sizeof(block_t);
    n->free  = 1;
    n->next  = b->next;
    n->prev  = b;
    if (b->next) b->next->prev = n;
    b->next  = n;
    b->size  = size;
    heap_used += sizeof(block_t);
}

void *kmalloc(size_t size) {
    if (!size) return NULL;
    /* Rounding up could overflow and turn a huge request into a tiny
       block - heap corruption would follow. */
    if (size > 0xFFFFFFF0u) return NULL;
    size = ALIGN8(size);
    if (size > heap_total) return NULL;
    u32 fl = irq_save();
    for (block_t *b = head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = 0;
            b->magic = MAGIC_USED;
            heap_used += b->size;
            irq_restore(fl);
            return (u8 *)b + sizeof(block_t);
        }
    }
    irq_restore(fl);
    return NULL;
}

void *kcalloc(size_t n, size_t size) {
    /* overflow guard: n * size could wrap to zero and hand out a tiny
       block for a multi-gigabyte request */
    if (n && size && n > (0xFFFFFFFFu / size)) return NULL;
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void merge(block_t *b) {
    while (b->next && b->next->free) {
        block_t *n = b->next;
        b->size += n->size + sizeof(block_t);
        b->next = n->next;
        if (n->next) n->next->prev = b;
        heap_used -= sizeof(block_t);
    }
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((u8 *)p - sizeof(block_t));
    if (b->magic != MAGIC_USED) { kprintf(T("[heap] block corruption at %p\n", "[heap] порча блока %p\n"), p); return; }
    u32 fl = irq_save();
    b->free = 1;
    b->magic = MAGIC_FREE;
    heap_used -= b->size;
    merge(b);
    if (b->prev && b->prev->free) merge(b->prev);
    irq_restore(fl);
}

char *kstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void heap_stats(u32 *total, u32 *used, u32 *blocks) {
    u32 n = 0;
    for (block_t *b = head; b; b = b->next) n++;
    if (total) *total = heap_total;
    if (used) *used = heap_used;
    if (blocks) *blocks = n;
}
