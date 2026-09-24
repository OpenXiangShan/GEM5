/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Deterministic mixed hot-set/stream test. Freestanding RISC-V Linux program:
 * riscv64-linux-gnu-gcc -O2 -static -nostdlib -ffreestanding -fno-pie \
 *     -no-pie -march=rv64g -mabi=lp64d -Wl,--no-relax,-e,_start \
 *     util/sdbp_reference/stream_hot.c -o /tmp/sdbp-stream-hot
 * Avoid compressed instructions: xs-dev's decoder assumes exact fetch PCs,
 * while TimingSimpleCPU fetches aligned words.
 */

#include <stdint.h>

#define HOT_LINES 256
#define STREAM_LINES 32768
#define WORDS_PER_LINE 8

static volatile uint64_t hot[HOT_LINES * WORDS_PER_LINE] __attribute__((aligned(4096)));
static volatile uint64_t stream[STREAM_LINES * WORDS_PER_LINE] __attribute__((aligned(4096)));

__attribute__((noinline)) static uint64_t
read_hot(unsigned line)
{
    return hot[line * WORDS_PER_LINE];
}

__attribute__((noinline)) static uint64_t
read_stream(unsigned line)
{
    return stream[line * WORDS_PER_LINE];
}

int
main(void)
{
    const unsigned rounds = 4;
    uint64_t sum = 0;
    uint64_t expected = 0;
    for (unsigned i = 0; i < HOT_LINES; ++i)
        hot[i * WORDS_PER_LINE] = i + 1;
    for (unsigned i = 0; i < STREAM_LINES; ++i)
        stream[i * WORDS_PER_LINE] = i + 7;
    for (unsigned r = 0; r < rounds; ++r) {
        for (unsigned chunk = 0; chunk < 128; ++chunk) {
            // Repeated hot passes teach reuse; each stream PC sees no reuse
            // within a cache generation. Periodic long bursts cause pressure.
            for (unsigned pass = 0; pass < 2; ++pass) {
                for (unsigned i = 0; i < HOT_LINES; ++i) {
                    sum += read_hot(i);
                    expected += i + 1;
                }
            }
            unsigned count = (chunk % 8 == 0) ? 1024 : 128;
            for (unsigned i = 0; i < count; ++i) {
                unsigned line = (chunk * 256 + i) % STREAM_LINES;
                sum += read_stream(line);
                expected += line + 7;
            }
        }
    }
    const char *message = sum == expected ? "checksum PASS\n" : "checksum FAIL\n";
    register long arg0 asm("a0") = 1;
    register const char *arg1 asm("a1") = message;
    register long arg2 asm("a2") = 14;
    register long number asm("a7") = 64;
    asm volatile("ecall" : "+r"(arg0) : "r"(arg1), "r"(arg2), "r"(number) : "memory");
    return sum != expected;
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
