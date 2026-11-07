/* Deterministic SE checks with independent expected values. */
#include <stdint.h>
#include <stdio.h>

#ifndef CASE
#define CASE 0
#endif
static volatile uint64_t data[32768] __attribute__((aligned(64)));

int main(void)
{
    uint64_t sum = 0, expected = 0;
    unsigned failures = 0;
#if CASE == 4
    for (unsigned i = 0; i < 8192; ++i) data[i] = i * 17u + 9u;
    __asm__ volatile("fence rw,rw" ::: "memory");
    for (unsigned i = 0; i < 16384; i += 8) {
        sum += data[((i + 0) * 1031u) & 8191u];
        sum += data[((i + 1) * 1031u) & 8191u];
        sum += data[((i + 2) * 1031u) & 8191u];
        sum += data[((i + 3) * 1031u) & 8191u];
        sum += data[((i + 4) * 1031u) & 8191u];
        sum += data[((i + 5) * 1031u) & 8191u];
        sum += data[((i + 6) * 1031u) & 8191u];
        sum += data[((i + 7) * 1031u) & 8191u];
    }
    /* 1031 is coprime to 8192; each pass visits every element once. */
    expected = 2 * (17ULL * 8191 * 8192 / 2 + 9ULL * 8192);
#else
    for (unsigned i = 0; i < 2048; ++i) {
        unsigned j = (i * 1031u) & 32767u;
#if CASE == 0
        /* Immediate full forwarding, mixed byte/word overlap, and fences. */
        uint64_t value = 0x1234000000000000ULL + i;
        data[j] = value;
        uint64_t got = data[j];
        failures += got != value;
        volatile uint8_t *bytes = (volatile uint8_t *)&data[j];
        bytes[3] = (uint8_t)(i ^ 0x5a);
        value = (value & ~(0xffULL << 24)) | ((uint64_t)(uint8_t)(i ^ 0x5a) << 24);
        got = data[j];
        failures += got != value;
        sum += got;
        expected += value;
        if ((i & 63) == 0) __asm__ volatile("fence rw,rw" ::: "memory");
#elif CASE == 1
        /* Two dispersed cache lines per iteration, dependent reloads. */
        unsigned k = (j + 17011u) & 32767u;
        data[j] = i * 17u + 9u;
        data[k] = i * 31u + 3u;
        __asm__ volatile("fence rw,rw" ::: "memory");
        uint64_t got = data[j] ^ data[k];
        uint64_t want = (i * 17u + 9u) ^ (i * 31u + 3u);
        failures += got != want;
        sum += got;
        expected += want;
#elif CASE == 2
        /* Alternating aliases/branches and a store address dependent on load. */
        unsigned k = (i & 1) ? j : ((j + 19) & 32767u);
        data[0] = k;
        unsigned address = (unsigned)data[0];
        data[address] = i + 0x100u;
        if (i & 1) {
            sum += data[k];
            expected += i + 0x100u;
        } else {
            data[j] = i + 7u;
            sum += data[j] + data[k];
            expected += (i + 7u) + (i + 0x100u);
        }
#else
        /* AMO aq/rl, LR/SC retry, and ordering with ordinary stores. */
        uint64_t old, status, got;
        data[j] = i;
        __asm__ volatile("amoadd.d.aqrl %0, %2, (%1)"
                         : "=r"(old) : "r"(&data[j]), "r"(3ULL) : "memory");
        failures += old != i;
        do {
            __asm__ volatile("lr.d.aq %0, (%2)\n\t"
                             "addi %0, %0, 5\n\t"
                             "sc.d.rl %1, %0, (%2)"
                             : "=&r"(got), "=&r"(status) : "r"(&data[j]) : "memory");
        } while (status);
        __asm__ volatile("fence rw,rw" ::: "memory");
        got = data[j];
        failures += got != i + 8u;
        sum += got;
        expected += i + 8u;
#endif
    }
#endif
    failures += sum != expected;
    printf("CASE=%d checksum=%llu expected=%llu failures=%u\n", CASE,
           (unsigned long long)sum, (unsigned long long)expected, failures);
    return failures ? 1 : 0;
}
