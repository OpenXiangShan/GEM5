#include <klib.h>

#ifndef PERF_NUM_LINES
#define PERF_NUM_LINES 65536U
#endif

#ifndef PERF_ROUNDS
#define PERF_ROUNDS 1U
#endif

#ifndef PERF_LINE_STRIDE
#define PERF_LINE_STRIDE 8191U
#endif

enum
{
    LINE_SIZE = 64,
    STORE_OFFSET = 7,
    VERIFY_SAMPLES = 16
};

#if PERF_NUM_LINES == 0 || (PERF_NUM_LINES & (PERF_NUM_LINES - 1)) != 0
#error "PERF_NUM_LINES must be a power of two"
#endif

#if PERF_ROUNDS == 0
#error "PERF_ROUNDS must be greater than zero"
#endif

#if (PERF_LINE_STRIDE & 1) == 0
#error "PERF_LINE_STRIDE must be odd"
#endif

static volatile uint8_t data[PERF_NUM_LINES * LINE_SIZE]
    __attribute__((aligned(LINE_SIZE)));

static inline uint64_t
read_mcycle(void)
{
    uint64_t value;
    asm volatile("csrr %0, mcycle" : "=r"(value));
    return value;
}

static inline uint64_t
read_minstret(void)
{
    uint64_t value;
    asm volatile("csrr %0, minstret" : "=r"(value));
    return value;
}

static uint8_t
store_value(unsigned int iteration, unsigned int round)
{
    return (uint8_t)(((iteration + 17U * round) % 251U) + 1U);
}

int
main(void)
{
    const unsigned int line_mask = PERF_NUM_LINES - 1U;
    uint64_t start_cycle;
    uint64_t start_inst;
    uint64_t end_cycle;
    uint64_t end_inst;

    printf("partial-store-perf: lines=%u rounds=%u stride=%u\n",
           PERF_NUM_LINES, PERF_ROUNDS, PERF_LINE_STRIDE);

    asm volatile("fence rw, rw" ::: "memory");
    start_cycle = read_mcycle();
    start_inst = read_minstret();

    for (unsigned int round = 0; round < PERF_ROUNDS; ++round) {
        unsigned int line = 0;
        for (unsigned int i = 0; i < PERF_NUM_LINES; ++i) {
            data[(size_t)line * LINE_SIZE + STORE_OFFSET] =
                store_value(i, round);
            line = (line + PERF_LINE_STRIDE) & line_mask;
        }
    }

    asm volatile("fence rw, rw" ::: "memory");
    end_cycle = read_mcycle();
    end_inst = read_minstret();

    printf("partial-store-perf: cycles=%lu instructions=%lu stores=%lu\n",
           (unsigned long)(end_cycle - start_cycle),
           (unsigned long)(end_inst - start_inst),
           (unsigned long)PERF_NUM_LINES * PERF_ROUNDS);

    for (unsigned int sample = 0; sample < VERIFY_SAMPLES; ++sample) {
        const unsigned int i = sample * (PERF_NUM_LINES / VERIFY_SAMPLES);
        const unsigned int line = (i * PERF_LINE_STRIDE) & line_mask;
        const uint8_t expected = store_value(i, PERF_ROUNDS - 1U);
        if (data[(size_t)line * LINE_SIZE + STORE_OFFSET] != expected ||
            data[(size_t)line * LINE_SIZE + STORE_OFFSET + 1] != 0) {
            printf("partial-store-perf: mismatch line=%u\n", line);
            return 1;
        }
    }

    printf("partial-store-perf: PASS\n");
    return 0;
}
