/* Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 * Build: riscv64-linux-gnu-gcc -nostdlib -static -march=rv64im -mabi=lp64
 *        -Wl,--build-id=none -o /tmp/sdbp-traffic traffic.S
 */
.section .text
.global _start
_start:
    /* Clean loads must also carry data when their L1 lines are evicted. */
    la t0, data
    li t1, 512
1:
    ld t2, 0(t0)
    bnez t2, fail
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 1b
    /* Fetch enough instructions to evict clean L1I and L3 lines. */
    .rept 2048
    nop
    .endr
#ifndef SDBP_READ_ONLY
    /* Store a different value in each line, then evict it from L1. */
    li s0, 4
2:
    la t0, data
    li t1, 512
3:
    add t2, t1, s0
    sd t2, 0(t0)
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 3b
    /* Exercise clean L1I victims, with writeback_clean disabled. */
    .rept 2048
    nop
    .endr
    la t0, data
    li t1, 512
4:
    ld t2, 0(t0)
    add t3, t1, s0
    bne t2, t3, fail
    /* A read of previously dirty data must already grant write permission. */
    sd t2, 0(t0)
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 4b
    addi s0, s0, -1
    bnez s0, 2b
#endif
    li a0, 0
    j done
fail:
    li a0, 1
done:
    li a7, 93
    ecall
.section .bss
.balign 4096
data:
    .skip 32768
