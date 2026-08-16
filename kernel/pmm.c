/* Побитовая карта свободных физических страниц (4 KiB) */
#include "kernel.h"

#define FRAME_SIZE 4096
#define MAX_FRAMES (1024 * 1024)          /* до 4 GiB */

static u32  frame_bitmap[MAX_FRAMES / 32];
static u32  total_frames = 0;
static u32  used_frames  = 0;

static inline void bm_set(u32 i)   { frame_bitmap[i >> 5] |=  (1u << (i & 31)); }
static inline void bm_clear(u32 i) { frame_bitmap[i >> 5] &= ~(1u << (i & 31)); }
static inline int  bm_test(u32 i)  { return (frame_bitmap[i >> 5] >> (i & 31)) & 1; }

struct mb_mmap {
    u32 size;
    u64 addr;
    u64 len;
    u32 type;
} __attribute__((packed));

void pmm_init(u32 mem_upper_kb, u32 mmap_addr, u32 mmap_len) {
    u32 mem_bytes = (mem_upper_kb + 1024) * 1024;
    total_frames = mem_bytes / FRAME_SIZE;
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;

    /* по умолчанию всё занято, освобождаем доступные регионы */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    used_frames = total_frames;

    if (mmap_addr && mmap_len) {
        struct mb_mmap *m = (struct mb_mmap *)mmap_addr;
        while ((u32)m < mmap_addr + mmap_len) {
            if (m->type == 1) {
                u32 base = (u32)m->addr;
                u32 end  = (u32)(m->addr + m->len);
                for (u32 a = (base + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1); a + FRAME_SIZE <= end; a += FRAME_SIZE) {
                    u32 idx = a / FRAME_SIZE;
                    if (idx < total_frames && bm_test(idx)) { bm_clear(idx); used_frames--; }
                }
            }
            m = (struct mb_mmap *)((u32)m + m->size + 4);
        }
    } else {
        for (u32 a = 0x100000; a + FRAME_SIZE <= mem_bytes; a += FRAME_SIZE) {
            u32 idx = a / FRAME_SIZE;
            if (idx < total_frames && bm_test(idx)) { bm_clear(idx); used_frames--; }
        }
    }

    /* резервируем первый мегабайт и само ядро */
    u32 kend = ((u32)&kernel_end + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
    for (u32 a = 0; a < kend; a += FRAME_SIZE) {
        u32 idx = a / FRAME_SIZE;
        if (idx < total_frames && !bm_test(idx)) { bm_set(idx); used_frames++; }
    }
}

u32 pmm_alloc_frame(void) {
    for (u32 i = 0; i < total_frames; i++) {
        if (!bm_test(i)) { bm_set(i); used_frames++; return i * FRAME_SIZE; }
    }
    return 0;
}

void pmm_free_frame(u32 addr) {
    u32 i = addr / FRAME_SIZE;
    if (i < total_frames && bm_test(i)) { bm_clear(i); used_frames--; }
}

/* Пометить диапазон занятым: под кучу и прочие статические области */
void pmm_reserve_range(u32 base, u32 size) {
    u32 start = base & ~(FRAME_SIZE - 1);
    u32 end   = (base + size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
    for (u32 a = start; a < end; a += FRAME_SIZE) {
        u32 i = a / FRAME_SIZE;
        if (i < total_frames && !bm_test(i)) { bm_set(i); used_frames++; }
    }
}

u32 pmm_total_frames(void) { return total_frames; }
u32 pmm_used_frames(void)  { return used_frames; }
u32 pmm_total_bytes(void)  { return total_frames * FRAME_SIZE; }
