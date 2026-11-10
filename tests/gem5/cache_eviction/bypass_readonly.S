/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Read-only bypass regression: no stack accesses or stores. Every victim
 * must remain clean, including after the LLC evicts its original copy.
 */

    .text
    .global _start
_start:
    li t4, 3
    li a0, 0
1:
    la t0, data
    li t1, 4096
2:
    ld t2, 0(t0)
    or a0, a0, t2
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 2b
    /* Fetch more code than L1I holds after data fills L2. */
    .rept 1024
    nop
    .endr
    addi t4, t4, -1
    bnez t4, 1b
    snez a0, a0
    li a7, 93
    ecall

    .bss
    .balign 4096
data:
    .skip 262144
