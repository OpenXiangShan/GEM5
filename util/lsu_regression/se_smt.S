/* Two independent SE processes exercise the two SMT LSQ thread identities. */
.option nopic
.option norelax
.section .text
.global _start
_start:
    la t0, values
    li t1, 256
loop:
    sd t1, 0(t0)
    addi t2, t1, 17
    sd t2, 64(t0)
    ld t3, 0(t0)
    bne t3, t1, fail
    ld t3, 64(t0)
    bne t3, t2, fail
    addi t1, t1, -1
    bnez t1, loop
    fence rw,rw
    li a0, 1
    la a1, message
    li a2, 9
    li a7, 64
    ecall
    li a0, 0
    li a7, 93
    ecall
fail:
    li a0, 1
    li a7, 93
    ecall
.section .data
message:
    .ascii "SMT_PASS\n"
.balign 64
values:
    .zero 128
