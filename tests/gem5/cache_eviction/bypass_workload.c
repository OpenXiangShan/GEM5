/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Freestanding RISC-V SE workload for L2 bypass and writeback checks.
 * Build with the same -nostdlib/-march=rv64g flags as stream_hot.c.
 */

#include <stdint.h>

#ifndef LINES
#define LINES 4096
#endif
#define WORDS_PER_LINE 8

static volatile uint64_t clean[LINES * WORDS_PER_LINE]
    __attribute__((aligned(4096)));
static volatile uint64_t dirty[LINES * WORDS_PER_LINE]
    __attribute__((aligned(4096)));

__attribute__((noinline)) static uint64_t
read_line(volatile uint64_t *data, unsigned line)
{
    return data[line * WORDS_PER_LINE];
}

__attribute__((noinline)) static void
write_line(unsigned line, uint64_t value)
{
    dirty[line * WORDS_PER_LINE] = value;
}

int
main(void)
{
    unsigned errors = 0;
    uint64_t sum = 0;
    // This footprint exceeds L1 but fits in the independent sampler.
    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned i = 0; i < 128; ++i)
            sum |= read_line(clean, i);
    }
    // Untouched lines stay clean even when granted writable permission.
    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned i = 0; i < LINES; ++i) {
            sum |= read_line(clean, i);
            uint64_t expected = round ? ((uint64_t)round << 32) + i : 0;
            errors += read_line(dirty, i) != expected;
            write_line(i, ((uint64_t)(round + 1) << 32) + i);
        }
        // Evict the modified lines from both private caches before reuse.
        for (unsigned i = 0; i < LINES; ++i)
            sum |= read_line(clean, i);
    }
    for (unsigned i = 0; i < LINES; ++i)
        errors += read_line(dirty, i) != ((uint64_t)3 << 32) + i;
    errors += sum != 0;

    const char *message = errors ? "bypass checksum FAIL\n" :
                                   "bypass checksum PASS\n";
    register long arg0 asm("a0") = 1;
    register const char *arg1 asm("a1") = message;
    register long arg2 asm("a2") = 21;
    register long number asm("a7") = 64;
    asm volatile("ecall" : "+r"(arg0) : "r"(arg1), "r"(arg2),
                 "r"(number) : "memory");
    return errors != 0;
}

asm(".text\n"
    ".global _start\n"
    "_start:\n"
    ".option push\n"
    ".option norelax\n"
    "la gp, __global_pointer$\n"
    ".option pop\n"
    "call main\n"
    "li a7, 93\n"
    "ecall\n");
