/* Check neighboring illegal encodings and existing SYSTEM instructions. */
.option norvc
.section .text
.global _start

.macro expect_illegal encoding
    la s4, illegal_\@
    li s5, \encoding
    addi s6, s2, 1
    li a0, 0x55
illegal_\@:
    .word \encoding
    bne s2, s6, fail
    li t0, 0x55
    bne a0, t0, fail
.endm

_start:
    la t0, trap
    csrw mtvec, t0
    csrw medeleg, zero
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x0f
    csrw pmpcfg0, t0

    /* Exercise register and immediate CSR forms, including zero operands. */
    csrw mscratch, zero
    li t1, 0x12
    csrrw t0, mscratch, t1
    bnez t0, fail
    li t1, 0x05
    csrrs t0, mscratch, t1
    li t2, 0x12
    bne t0, t2, fail
    li t1, 0x03
    csrrc t0, mscratch, t1
    li t2, 0x17
    bne t0, t2, fail
    csrrwi t0, mscratch, 5
    li t2, 0x14
    bne t0, t2, fail
    csrrsi t0, mscratch, 2
    li t2, 5
    bne t0, t2, fail
    csrrci t0, mscratch, 1
    li t2, 7
    bne t0, t2, fail
    csrr t0, mscratch
    li t2, 6
    bne t0, t2, fail
    csrrsi zero, mscratch, 0
    csrrci t0, mscratch, 0
    bne t0, t2, fail

    li s2, 0
    /* Each encoding violates a fixed MOP field; none matches REF H ops. */
    expect_illegal 0x81804573 /* MOP.R: bit 22 cleared */
    expect_illegal 0xa1c04573 /* MOP.R: bit 29 set */
    expect_illegal 0x92004573 /* MOP.RR: bit 28 set */
    expect_illegal 0x02004573 /* MOP.RR: bit 31 cleared */
    expect_illegal 0x82000573 /* MOP.RR: FUNCT3 changed to 0 */
    li t0, 5
    bne s2, t0, fail
    li a0, 0
    .word 0x0000006b
fail:
    j fail

.balign 4
trap:
    csrr t0, mcause
    li t1, 2
    bne t0, t1, fail
    csrr t0, mepc
    bne t0, s4, fail
    csrr t1, mtval
    bne t1, s5, fail
    addi t0, t0, 4
    csrw mepc, t0
    addi s2, s2, 1
    mret
