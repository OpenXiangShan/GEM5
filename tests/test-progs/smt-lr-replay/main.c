#include <stdint.h>

extern int printf_(const char *, ...);

#ifndef MODE
#define MODE 0
#endif
#ifndef ROUNDS
#define ROUNDS 2000
#endif
#ifndef DIVS
#define DIVS 4
#endif
#ifndef WRITER_NOPS
#define WRITER_NOPS 0
#endif
#ifndef READ_OP
#define READ_OP "lr.w"
#endif

static volatile uint32_t target __attribute__((aligned(128)));
static volatile uint32_t ready __attribute__((aligned(128)));
static volatile uint32_t start __attribute__((aligned(128)));
static volatile uint32_t request __attribute__((aligned(128)));
static volatile uint32_t ack __attribute__((aligned(128)));
static volatile uint32_t done __attribute__((aligned(128)));

static inline void fence_all(void)
{
    asm volatile("fence rw, rw" ::: "memory");
}

static void finish(unsigned code)
{
    asm volatile("mv a0, %0; .word 0x0000006b" :: "r"(code) : "a0", "memory");
    while (1) {}
}

int main(void)
{
    unsigned long id;
    asm volatile("csrr %0, mhartid" : "=r"(id));
    if (id == 0) {
        *(volatile uint64_t *)0x39001008 = 0;
        while (!ready) {}
        fence_all();
        start = 1;
        fence_all();
        unsigned errors = 0;
        uint32_t last = 0;
        unsigned changes = 0;
        for (uint32_t i = 1; i <= ROUNDS; ++i) {
            uint32_t loaded;
#if MODE == 0
            unsigned long delayed;
            /* The older independent divide chain holds retirement while LR
             * can issue. Each writer value is unique and increases strictly. */
            asm volatile(
                "mv %0, %2\n"
                ".rept %c4\n"
                "divu %0, %0, %3\n"
                ".endr\n"
                READ_OP " %1, (%5)\n"
                : "=&r"(delayed), "=&r"(loaded)
                : "r"(~(unsigned long)i), "r"(3UL), "i"(DIVS), "r"(&target)
                : "memory");
            errors += loaded < last || loaded > 8 * ROUNDS;
            changes += loaded != last;
            last = loaded;
            (void)delayed;
#elif MODE == 1
            /* Publish the request after LR. The acknowledgement is published
             * after the other hart's overlapping store, before our SC. */
            target = 7;
            fence_all();
            asm volatile(READ_OP " %0, (%1)" : "=&r"(loaded) : "r"(&target) : "memory");
            fence_all();
            request = i;
            while (ack != i) {}
            fence_all();
            uint32_t failed;
            asm volatile("sc.w %0, %2, (%1)" : "=&r"(failed)
                         : "r"(&target), "r"(9U) : "memory");
            fence_all();
            errors += loaded != 7 || failed == 0 || target != 8;
#else
            /* The writer is unable to overwrite this round until the next
             * request. Acquire after ack makes the expected LR value unique. */
            request = i;
            while (ack != i) {}
            fence_all();
            asm volatile(READ_OP " %0, (%1)" : "=&r"(loaded) : "r"(&target) : "memory");
            errors += loaded != i;
            fence_all();
#endif
        }
        while (!done) {}
        fence_all();
        printf_("LR_PROBE mode=%u rounds=%u errors=%u changes=%u last=%u target=%u\n",
               MODE, ROUNDS, errors, changes, last, target);
        finish(errors != 0);
    }
    if (id == 1) {
        ready = 1;
        fence_all();
        while (!start) {}
        fence_all();
#if MODE == 0
        for (uint32_t i = 1; i <= 8 * ROUNDS; ++i) {
            target = i;
            asm volatile(".rept %c0; nop; .endr" :: "i"(WRITER_NOPS) : "memory");
        }
#else
        for (uint32_t i = 1; i <= ROUNDS; ++i) {
            while (request != i) {}
            fence_all();
            target = MODE == 1 ? 8 : i;
            fence_all();
            ack = i;
            fence_all();
        }
#endif
        fence_all();
        done = 1;
        fence_all();
    }
    while (1) {}
    return 0;
}
