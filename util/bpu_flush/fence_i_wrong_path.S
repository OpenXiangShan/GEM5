    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function

_start:
    /* A cold conditional predictor fetches the sequential path first. */
    beq     zero, zero, pass
    fence.i

fail:
    j       fail

pass:
    li      a0, 0
    .word   0x4200007b
1:
    j       1b

    .section .note.GNU-stack, "", @progbits
