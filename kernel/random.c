/* ============================================================
 *  KvantOS - kernel random number generator
 *
 *  A small xorshift32 generator shared by the shell utilities
 *  (rand, cal decorations, matrix rain, fortunes...). It is
 *  seeded from the timer at first use, so every boot produces
 *  a different sequence.
 * ============================================================ */
#include "kernel.h"

static u32 rnd_state = 0;

/* Seed the generator. A zero seed is rejected: xorshift with a
   zero state would stay zero forever. */
void kv_rand_seed(u32 seed) {
    rnd_state = seed ? seed : 0x9E3779B9u;
}

u32 kv_rand(void) {
    if (!rnd_state)
        rnd_state = (u32)timer_ticks() ^ 0x2545F491u;
    u32 x = rnd_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rnd_state = x;
    return x;
}

/* A number in [0, n). n == 0 yields 0. */
u32 kv_rand_max(u32 n) {
    if (!n) return 0;
    return kv_rand() % n;
}
