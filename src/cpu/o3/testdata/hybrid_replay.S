#ifndef HYBRID_REPLAY_SIMPLE_SLOT
#define HYBRID_REPLAY_SIMPLE_SLOT 1
#endif

.option norvc
.section .text
.globl _start
_start:
    la s0, data
    li s1, 3000
    li s2, 1
    li s3, 7
#if HYBRID_REPLAY_SIMPLE_SLOT
    li t2, 0
#endif
loop:
    # Delay the older store address while the younger load can execute.
    div t0, s2, s3
    div t0, t0, s3
    add t0, s0, t0
    sd s2, 0(t0)
    # Give the replaying load a simple former slot when the batch permits it.
#if HYBRID_REPLAY_SIMPLE_SLOT
    addi t2, t2, 1
#endif
    ld t1, 0(s0)
    bne t1, s2, fail
    addi s1, s1, -1
    xori s2, s2, 1
    bnez s1, loop
pass:
    li a0, 0
    .word 0x0000006b
fail:
    li a0, 1
    .word 0x0000006b
.section .data
.balign 8
data:
    .dword 0
