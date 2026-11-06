#include <stdint.h>

extern int printf_(const char *, ...);
#ifndef ROUNDS
#define ROUNDS 1000
#endif

static volatile uint32_t target __attribute__((aligned(128)));
static volatile uint32_t ready __attribute__((aligned(128)));
static volatile uint32_t start __attribute__((aligned(128)));
static volatile uint32_t done __attribute__((aligned(128)));
static volatile uint64_t other_sum __attribute__((aligned(128)));

static inline void fence_all(void)
{
    asm volatile("fence rw, rw" ::: "memory");
}

static __attribute__((noinline)) uint64_t decrement_many(void)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < ROUNDS; ++i) {
        uint32_t old;
        asm volatile("amoadd.w %0, %2, (%1)" : "=&r"(old)
                     : "r"(&target), "r"(-1) : "memory");
        sum += old;
    }
    return sum;
}

int main(void)
{
    unsigned long id;
    asm volatile("csrr %0, mhartid" : "=r"(id));
    if (id == 0) {
        *(volatile uint64_t *)0x39001008 = 0;
        target = 2 * ROUNDS + 9;
        while (!ready) {}
        fence_all();
        start = 1;
        fence_all();
        uint64_t sum = decrement_many();
        while (!done) {}
        fence_all();
        uint32_t loaded;
        asm volatile("lr.w %0, (%1)" : "=&r"(loaded)
                     : "r"(&target) : "memory");
        /* Across both harts the old values must sum the descending sequence
         * from 2 * ROUNDS + 9 through 10, and final memory must be 9. */
        uint64_t n = 2 * ROUNDS;
        unsigned errors = loaded != 9 || sum + other_sum != n * (n + 19) / 2;
        printf_("GOLDEN_PROBE mode=3 rounds=%u errors=%u target=%u\n",
               ROUNDS, errors, target);
        asm volatile("mv a0, %0; .word 0x0000006b"
                     :: "r"(errors != 0) : "a0", "memory");
    } else if (id == 1) {
        ready = 1;
        fence_all();
        while (!start) {}
        fence_all();
        other_sum = decrement_many();
        fence_all();
        done = 1;
        fence_all();
    }
    while (1) {}
}
