.option norvc
.section .text
.globl _start
_start:
    la t0, handler
    csrw mtvec, t0
    li t0, 0x6000
    csrc mstatus, t0
    li s0, 0
    li s1, 0
loop:
    addi s1, s1, 1
    addi s2, s1, 2
    fadd.s f0, f1, f2
    addi s3, s2, 3
    ecall
    addi s4, s3, 4
    bne s0, s1, loop
    j loop
.align 2
handler:
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    addi s0, s0, 1
    mret
