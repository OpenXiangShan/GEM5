/* Scalar APF smoke: a difficult branch, compressed instructions and calls. */
.section .text
.globl _start
.option rvc
_start:
    la sp, stack_top
    li s0, 0x1234567
    li s1, 20000
    li s2, 0
.balign 64
loop:
    slli t0, s0, 13
    xor s0, s0, t0
    srli t0, s0, 17
    xor s0, s0, t0
    slli t0, s0, 5
    xor s0, s0, t0
    li t1, 65521
    remu t2, s0, t1
    andi t2, t2, 1
    beqz t2, alternate
    .rept 8
    addi s2, s2, 1
    .endr
    call subroutine
    j join
.balign 32
alternate:
    .rept 10
    addi s2, s2, 2
    .endr
    call subroutine
join:
#ifdef APF_FENCE
#ifndef APF_FENCE_DENSE
    /* Leave recovery opportunities between instruction-consistency events. */
    andi t6, s1, 63
    bnez t6, fences_done
#endif
    fence.i
    sfence.vma
fences_done:
#endif
    addi s1, s1, -1
    bnez s1, loop
    li a0, 0
    .word 0x0005006b
1:  j 1b
.balign 4096
.space 4094, 0
subroutine:
    /* The first 32-bit instruction crosses a page boundary. */
.option push
.option norvc
    addi s2, s2, 3
.option pop
    .rept 8
    addi s2, s2, 3
    .endr
#ifdef APF_AMO
    la t3, scratch
    li t4, 1
    amoadd.d.aqrl t5, t4, (t3)
#endif
    ret
.section .bss
.balign 16
scratch:
    .skip 8
stack:
    .skip 4096
stack_top:
