#include <stdint.h>

extern int printf_(const char *, ...);
static volatile uint32_t target __attribute__((aligned(128)));

int main(void)
{
    unsigned long id;
    asm volatile("csrr %0, mhartid" : "=r"(id));
    if (id == 0) {
        *(volatile uint64_t *)0x39001008 = 0;
        target = 9;
        /* Enable vector state before executing a store with no active bytes.
         * It must complete without writing memory or dereferencing a request
         * that the predicated-off path never created. */
        asm volatile("csrs mstatus, %0" :: "r"(3UL << 9) : "memory");
        asm volatile(".option push; .option arch,+v; "
                     "vsetivli zero,0,e8,m1,ta,ma; vse8.v v0,(%0); .option pop"
                     :: "r"(&target) : "memory");
        unsigned errors = target != 9;
        printf_("GOLDEN_PROBE mode=4 rounds=1 errors=%u target=%u\n",
               errors, target);
        asm volatile("mv a0, %0; .word 0x0000006b"
                     :: "r"(errors != 0) : "a0", "memory");
    }
    while (1) {}
}
