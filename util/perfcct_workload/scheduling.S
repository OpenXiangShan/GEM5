/* Equal DIV counts with either operand dependence or independent inputs.
 * XOR restores the dividend after every DIV, so both variants use the same
 * arithmetic operands and differ only in register dependency edges. */
    .option norvc
    .option norelax
    .section .text
    .globl _start
_start:
    li t0, 3
    li t1, 0x123456789abcdef
    li t2, 0x14252450a92894a
    li s1, 1
    li s2, 0
    jal ra, probe_div_0
    li s1, 16
    li s2, 0
    fence rw, rw
    .globl roi_begin
roi_begin:
    addi s4, zero, 1
    jal ra, probe_div_0
    .globl roi_end
roi_end:
    addi s4, zero, 2
    li t3, 0x91a2b3c4d5e6f780
    bne s2, t3, fail
    li a0, 0
    j exit
fail:
    li a0, 1
exit:
    li a7, 93
    ecall
    unimp
    .globl probe_div_0
probe_div_0:
    divu a0, t1, t0
    xor a0, a0, t2
    .globl probe_div_1
probe_div_1:
#if SERIAL
    divu a1, a0, t0
#else
    divu a1, t1, t0
#endif
    xor a1, a1, t2
    .globl probe_div_2
probe_div_2:
#if SERIAL
    divu a2, a1, t0
#else
    divu a2, t1, t0
#endif
    xor a2, a2, t2
    .globl probe_div_3
probe_div_3:
#if SERIAL
    divu a3, a2, t0
#else
    divu a3, t1, t0
#endif
    xor a3, a3, t2
    .globl probe_div_4
probe_div_4:
#if SERIAL
    divu a4, a3, t0
#else
    divu a4, t1, t0
#endif
    xor a4, a4, t2
    .globl probe_div_5
probe_div_5:
#if SERIAL
    divu a5, a4, t0
#else
    divu a5, t1, t0
#endif
    xor a5, a5, t2
    .globl probe_div_6
probe_div_6:
#if SERIAL
    divu a6, a5, t0
#else
    divu a6, t1, t0
#endif
    xor a6, a6, t2
    .globl probe_div_7
probe_div_7:
#if SERIAL
    divu a7, a6, t0
#else
    divu a7, t1, t0
#endif
    xor a7, a7, t2
    add a0, a0, a1
    add a2, a2, a3
    add a4, a4, a5
    add a6, a6, a7
    add a0, a0, a2
    add a4, a4, a6
    add a0, a0, a4
    add s2, s2, a0
    addi s1, s1, -1
    bnez s1, probe_div_0
    ret
    .section .note.GNU-stack,"",@progbits
