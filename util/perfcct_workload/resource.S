/* Eight independent cold loads per batch: separate lines or one line.
 * Capacity interventions use the same binary and change only cache params. */
    .option norvc
    .option norelax
    .section .text
    .globl _start
_start:
    la s0, data
    li s1, 16
    li s2, 0
    fence rw, rw
    .globl roi_begin
roi_begin:
    addi s4, zero, 1
    .globl probe_load
probe_load:
    ld a0, 0*STRIDE(s0)
    ld a1, 1*STRIDE(s0)
    ld a2, 2*STRIDE(s0)
    ld a3, 3*STRIDE(s0)
    ld a4, 4*STRIDE(s0)
    ld a5, 5*STRIDE(s0)
    ld a6, 6*STRIDE(s0)
    ld a7, 7*STRIDE(s0)
    add a0, a0, a1
    add a2, a2, a3
    add a4, a4, a5
    add a6, a6, a7
    add a0, a0, a2
    add a4, a4, a6
    add a0, a0, a4
    add s2, s2, a0
    addi s0, s0, 8*STRIDE
    addi s1, s1, -1
    bnez s1, probe_load
    .globl roi_end
roi_end:
    addi s4, zero, 2
    li t0, 128
    bne s2, t0, fail
    li a0, 0
    j exit
fail:
    li a0, 1
exit:
    li a7, 93
    ecall
    unimp

    .section .data
    .balign 4096
data:
    .rept 16*STRIDE
    .quad 1
    .endr
    .section .note.GNU-stack,"",@progbits
