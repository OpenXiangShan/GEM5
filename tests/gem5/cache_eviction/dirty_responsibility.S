/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * No stack or implicit stores: each m5_exit separates an observable phase.
 */

    .macro phase
    li a0, 0
    .long 0x4200007b
    .endm

    .macro evict_l1
    la t0, scratch
    li t1, 64
1:
    ld t2, 0(t0)
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 1b
    .endm

    .text
    .global _start
_start:
    la s0, target
    li t0, 42
    sd t0, 0(s0)
    evict_l1
    /* Phase 0: L1 wrote back 42; L2 is dirty and has the latest data. */
    phase

    ld t0, 0(s0)
    li t1, 42
    bne t0, t1, fail
    evict_l1
    /* Phase 1: a read-only L1 refill/eviction must not return dirty data. */
    phase

    ld t0, 0(s0)
    li t1, 42
    bne t0, t1, fail
    li t0, 43
    sd t0, 0(s0)
    /* Phase 2: L1 has 43 while the retained dirty L2 copy still has 42. */
    phase

    /* Instruction fetches evict L2 without replacing the target in L1D. */
    .rept 16384
    nop
    .endr
    ld t0, 0(s0)
    li t1, 43
    bne t0, t1, fail
    /* Phase 3: the victim probe must have recovered 43, never stale 42. */
    phase

    evict_l1
    /* Phase 4: the dirty LLC -> L2 -> L1 read must also leave L1 clean. */
    phase

    li a0, 0
    li a7, 93
    ecall
fail:
    li a0, 1
    li a7, 93
    ecall

    .bss
    .balign 4096
target:
    .skip 64
    .balign 4096
scratch:
    .skip 4096
