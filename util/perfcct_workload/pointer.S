/* Warm the same loop using either a separate chain or the measured chain. */
    .option norvc
    .option norelax
    .section .text
    .globl _start
_start:
    la s0, chain_a
    li s3, WARM
    beqz s3, prewarm
    la s0, chain_b
prewarm:
    li s1, 64
    li s2, 0
    jal ra, probe_load
setup:
    la s0, chain_b
    li s1, 64
    li s2, 0
    fence rw, rw
    .globl roi_begin
roi_begin:
    addi s4, zero, 1
    jal ra, probe_load
    .globl roi_end
roi_end:
    addi s4, zero, 2
    la t0, chain_b
    bne s0, t0, fail
    li t0, 64
    bne s2, t0, fail
    li a0, 0
    j exit
fail:
    li a0, 1
exit:
    li a7, 93
    ecall
    unimp
    .globl probe_load
probe_load:
    ld s0, 0(s0)
    addi s2, s2, 1
    addi s1, s1, -1
    bnez s1, probe_load
    ret

    .section .data
    .balign 4096
chain_a:
    .rept 63
    .quad . + 64
    .zero 56
    .endr
    .quad chain_a
    .zero 56
    .balign 4096
chain_b:
    .rept 63
    .quad . + 64
    .zero 56
    .endr
    .quad chain_b
    .zero 56
    .section .note.GNU-stack,"",@progbits
