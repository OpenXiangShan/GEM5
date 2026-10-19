    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function

_start:
    li t1, 0

    fence.i

    /* Keep running long enough to observe the full 516-cycle window. */
    .rept 2048
    addi t1, t1, 1
    .endr

    li a0, 0
    .word 0x4200007b

1:
    j 1b

    .size _start, . - _start
    .section .note.GNU-stack, "", @progbits
