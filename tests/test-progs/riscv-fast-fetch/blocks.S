.section .text
.global _start
.option rvc
_start:
    li s0, 1024
    li s1, 0x13579
    li s2, 0
    .ifdef TEST_TRAP
    lla t0, trap
    csrw mtvec, t0
    .endif

driver:
    .ifdef TEST_TRAP
    .2byte 0
    .endif
    slli t0, s1, 13
    xor s1, s1, t0
    srli t0, s1, 17
    xor s1, s1, t0
    slli t0, s1, 5
    xor s1, s1, t0
    andi t0, s1, 1
    bnez t0, block7
    c.addi s2, 1
    c.j block7

.balign 64
block7:
    .rept 6
    c.addi s2, 1
    .endr
    c.j block8

.balign 64
block8:
    .rept 7
    c.addi s2, 1
    .endr
    c.j block9

.balign 64
block9:
    .rept 8
    c.addi s2, 1
    .endr
    c.j block12

.balign 64
block12:
    .option push
    .option norvc
    .rept 11
    addi s2, s2, 1
    .endr
    .option pop
    c.j block16

.balign 64
block16:
    .rept 15
    c.addi s2, 1
    .endr
    c.j finish

.balign 64
finish:
    addi s0, s0, -1
    bnez s0, driver
    li a0, 0
    .word 0x0000006b

.ifdef TEST_TRAP
.balign 4
trap:
    csrr t0, mepc
    addi t0, t0, 2
    csrw mepc, t0
    mret
.endif
