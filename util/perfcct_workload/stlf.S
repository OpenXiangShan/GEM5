/* Ready store address, delayed store data, and a younger aliasing load. */
    .option norvc
    .option norelax
    .section .text
    .globl _start
_start:
    la s0, slot
    ld t2, 0(s0)
    li t0, 3
    li t1, 0x123456789abcdef
    li s1, 16
    li s2, EXPECTED
    fence rw, rw
    .globl roi_begin
roi_begin:
    addi s4, zero, 1
loop:
    mv a0, t1
    .rept DEPTH
    divu a0, a0, t0
    xor a0, a0, t1
    .endr
    .globl probe_store
probe_store:
    sd a0, 0(s0)
    .globl probe_load
probe_load:
    ld a1, 0(s0)
    bne a1, s2, fail
    addi s1, s1, -1
    bnez s1, loop
    .globl roi_end
roi_end:
    addi s4, zero, 2
    li a0, 0
    j exit
fail:
    li a0, 1
exit:
    li a7, 93
    ecall
    unimp

    .section .data
    .balign 64
slot:
    .quad 0
    .section .note.GNU-stack,"",@progbits
