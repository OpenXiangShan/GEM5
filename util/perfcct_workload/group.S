/* Run the identical binary with and without ROB commit grouping. */
    .option norvc
    .option norelax
    .section .text
    .globl _start
_start:
    li t0, 3
    li t1, 0x123456789abcdef
    li s1, 32
    li s2, 0
    fence rw, rw
    .globl roi_begin
roi_begin:
    addi s4, zero, 1
    .balign 32
    .globl probe_ready
probe_ready:
    addi s2, s2, 7
    .globl probe_div
probe_div:
    divu a0, t1, t0
    addi s3, zero, 9
    xor a0, a0, t1
    addi s1, s1, -1
    bnez s1, probe_ready
    .globl roi_end
roi_end:
    addi s4, zero, 2
    li t2, 0x14252450a92894a
    bne a0, t2, fail
    li t2, 224
    bne s2, t2, fail
    li a0, 0
    j exit
fail:
    li a0, 1
exit:
    li a7, 93
    ecall
    unimp
    .section .note.GNU-stack,"",@progbits
