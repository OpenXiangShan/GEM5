/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Read dirty LLC data without stores, then force it through memory and verify.
 * There is no stack and the only stores occur before the first m5_exit.
 */

    .macro phase
    li a0, 0
    .long 0x4200007b
    .endm

    .macro read_lines area, count
    la t0, \area
    li t1, \count
1:
    ld t2, 0(t0)
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 1b
    .endm

    .macro check_target
    la t0, target
    li t1, 512
    li t3, 42
1:
    ld t2, 0(t0)
    bne t2, t3, fail
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 1b
    .endm

    .text
    .global _start
_start:
    /* Fill L2 tags before writing a footprint that fits in the LLC. */
    read_lines scratch, 512
    la t0, target
    li t1, 512
    li t2, 42
1:
    sd t2, 0(t0)
    addi t0, t0, 64
    addi t1, t1, -1
    bnez t1, 1b
    read_lines scratch, 512
    phase

    /* Phases 1/2 must inherit dirty ownership without executing a store. */
    check_target
    read_lines scratch, 512
    phase
    check_target
    read_lines scratch, 512
    phase

    /* Force dirty responsibility to survive eviction beyond LLC capacity. */
    read_lines flush, 4096
    phase
    check_target
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
scratch:
    .skip 32768
    .balign 4096
target:
    .skip 32768
    .balign 4096
flush:
    .skip 262144
