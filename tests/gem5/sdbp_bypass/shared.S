/* Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 * Build twice, with -DROLE=0 and -DROLE=1, using the traffic.S toolchain.
 * coherence.py maps the same physical page at 0x20000000 in both processes.
 */
.section .text
.global _start
_start:
    li s0, 0x20000000
    li s1, 1
    li s2, 257
1:
    ld t0, 0(s0)
    li t1, ROLE
    bne t0, t1, 1b
    fence rw, rw
#if ROLE == 0
    ld t0, 64(s0)
    addi t1, s1, -1
    bne t0, t1, fail
    sd s1, 64(s0)
#else
    ld t0, 64(s0)
    bne t0, s1, fail
#endif
    fence rw, rw
    li t0, 1 - ROLE
    sd t0, 0(s0)
    addi s1, s1, 1
    bne s1, s2, 1b
#if ROLE == 0
2:
    ld t0, 0(s0)
    bnez t0, 2b
#endif
    li a0, 0
    j done
fail:
    li a0, 1
    sd a0, 128(s0)
    li a7, 94
    ecall
done:
    li a7, 93
    ecall
