/* Test cache-line zeroing, neighboring data, and S-mode CBZE checks. */
.section .text
.global _start
_start:
    .ifdef TEST_S
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x0f
    csrw pmpcfg0, t0
    la t0, trap
    csrw mtvec, t0
    .ifdef TEST_DISABLED
    li t0, 0
    .else
    li t0, 0x80
    .endif
    csrw menvcfg, t0
    csrw senvcfg, t0
    la t0, smode
    csrw mepc, t0
    li t0, 0x800
    csrw mstatus, t0
    mret
    .endif
smode:
    li t0, 0x80010000
    li t1, -1
    li t2, 24
fill:
    sd t1, 0(t0)
    addi t0, t0, 8
    addi t2, t2, -1
    bnez t2, fill
    fence rw,rw
    li a0, 0x80010051
cbo_pc:
    .word 0x0045200f
    .ifdef TEST_DISABLED
    j fail
    .endif
    li t0, 0x80010000
    li t2, 24
verify:
    ld t3, 0(t0)
    li t4, 0x80010040
    bltu t0, t4, outside
    li t4, 0x80010080
    bgeu t0, t4, outside
    bnez t3, fail
    j next
outside:
    bne t3, t1, fail
next:
    addi t0, t0, 8
    addi t2, t2, -1
    bnez t2, verify
    li a0, 0
    .word 0x0000006b
fail:
    j fail

.balign 4
trap:
    .ifdef TEST_DISABLED
    csrr t0, mcause
    li t1, 2
    bne t0, t1, fail
    csrr t0, mepc
    la t1, cbo_pc
    bne t0, t1, fail
    li t0, 0x80010040
    ld t1, 0(t0)
    li t2, -1
    bne t1, t2, fail
    li a0, 0
    .word 0x0000006b
    .else
    j fail
    .endif
