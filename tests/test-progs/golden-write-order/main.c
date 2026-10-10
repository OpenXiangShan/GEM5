#include <stdint.h>

extern int printf_(const char *, ...);
#ifndef MODE
#define MODE 0
#endif
#ifndef ROUNDS
#define ROUNDS 32
#endif
#ifndef DELAY
#define DELAY 0
#endif

static volatile struct {
    uint32_t value;
    uint32_t guard;
} target __attribute__((aligned(128)));
static volatile uint32_t ready __attribute__((aligned(128)));
static volatile uint32_t request __attribute__((aligned(128)));
static volatile uint32_t ack __attribute__((aligned(128)));
static volatile uint32_t other_old __attribute__((aligned(128)));
static volatile uint32_t done __attribute__((aligned(128)));

static inline void fence_all(void)
{
    asm volatile("fence rw, rw" ::: "memory");
}

static inline uint32_t decrement(void)
{
    uint32_t old;
    asm volatile("amoadd.w %0, %2, (%1)" : "=&r"(old)
                 : "r"(&target.value), "r"(-1) : "memory");
    return old;
}

int main(void)
{
    unsigned long id;
    asm volatile("csrr %0, mhartid" : "=r"(id));
    if (id == 0) {
        *(volatile uint64_t *)0x39001008 = 0;
        target.guard = 0x13579bdf;
        while (!ready) {}
        unsigned errors = 0;
        for (uint32_t i = 1; i <= ROUNDS; ++i) {
            target.value = 9;
            fence_all();
            request = i;
            fence_all();
            asm volatile(".rept %c0; nop; .endr" :: "i"(DELAY) : "memory");
            uint32_t old = decrement();
            while (ack != i) {}
            fence_all();
            uint32_t loaded;
            asm volatile("lr.w %0, (%1)" : "=&r"(loaded)
                         : "r"(&target.value) : "memory");
#if MODE == 0
            errors += loaded != 7 || old + other_old != 17;
#elif MODE == 1
            /* Both serialization orders are legal, but the returned old
             * values and final memory must agree on the same order. */
            errors += !((old == 9 && other_old == 8 && loaded == 32) ||
                        (old == 32 && other_old == 9 && loaded == 31));
#else
            errors += !((old == 9 && loaded == 32) ||
                        (old == 32 && loaded == 31));
#endif
            errors += target.guard != 0x13579bdf;
            fence_all();
        }
        while (!done) {}
        fence_all();
        printf_("GOLDEN_PROBE mode=%u rounds=%u errors=%u target=%u guard=%x\n",
               MODE, ROUNDS, errors, target.value, target.guard);
        asm volatile("mv a0, %0; .word 0x0000006b"
                     :: "r"(errors != 0) : "a0", "memory");
    } else if (id == 1) {
        ready = 1;
        fence_all();
        for (uint32_t i = 1; i <= ROUNDS; ++i) {
            while (request != i) {}
            fence_all();
#if MODE == 0
            other_old = decrement();
#elif MODE == 1
            uint32_t old;
            asm volatile("amoswap.w %0, %2, (%1)" : "=&r"(old)
                         : "r"(&target.value), "r"(32) : "memory");
            other_old = old;
#else
            target.value = 32;
#endif
            fence_all();
            ack = i;
            fence_all();
        }
        done = 1;
        fence_all();
    }
    while (1) {}
}
