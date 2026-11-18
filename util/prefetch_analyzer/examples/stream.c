// Copyright (c) 2026 OpenXiangShan
// SPDX-License-Identifier: BSD-3-Clause
#include <stdint.h>
#include <stdio.h>

static volatile uint64_t input[65536] __attribute__((aligned(4096)));
static volatile uint64_t output[65536] __attribute__((aligned(4096)));

int
main(void)
{
    uint64_t sum = 1;
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned i = 0; i < 65536; i += 8) {
            sum += input[i];
            // Give a correct stride predictor time to fetch the next line.
            for (unsigned k = 0; k < 8; ++k) {
                sum ^= sum >> 13;
                sum *= 0x5bd1e995;
            }
            output[i] = sum;
        }
    }
    printf("checksum=%llu\n", (unsigned long long)sum);
    return 0;
}
