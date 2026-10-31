/* Raw M-mode image: enable VS, cross cache lines, check every output byte.
 * Only the success path reaches the custom 0x6b exit instruction.
 * Failure loops until the instruction limit, which the runner rejects.
 */
.section .text.init
.global _start
_start:
    la t0, failed
    csrw mtvec, t0
    li t0, (3 << 9)
    csrs mstatus, t0
    la s0, source
    la s1, target
    addi s0, s0, 56
    addi s1, s1, 56
    li t0, 0
    li t1, 16
1:
    addi t2, t0, 16
    add t3, s0, t0
    sb t2, 0(t3)
    addi t0, t0, 1
    bltu t0, t1, 1b
    fence rw,rw
    li s2, 64
2:
    vsetivli zero, 16, e8, m1, ta, ma
    vle8.v v8, (s0)
    vse8.v v8, (s1)
    fence rw,rw
    li t0, 0
3:
    add t3, s1, t0
    lbu t2, 0(t3)
    addi t4, t0, 16
    bne t2, t4, failed
    addi t0, t0, 1
    bltu t0, t1, 3b
#ifdef TEST_MASKED
    /* Update even lanes only; odd bytes must retain the unmasked value. */
    vid.v v9
    vand.vi v9, v9, 1
    vmseq.vi v0, v9, 0
    vadd.vi v8, v8, 7
    vse8.v v8, (s1), v0.t
    fence rw,rw
    li t0, 0
5:
    add t3, s1, t0
    lbu t2, 0(t3)
    addi t4, t0, 16
    andi t5, t0, 1
    bnez t5, 6f
    addi t4, t4, 7
6:
    bne t2, t4, failed
    addi t0, t0, 1
    bltu t0, t1, 5b
#endif
    lbu t2, -1(s1)
    bnez t2, failed
    lbu t2, 16(s1)
    bnez t2, failed
    addi s2, s2, -1
    bnez s2, 2b
    li a0, 0
    .word 0x0000006b
4:
    j 4b
.balign 4
failed:
    li a0, 1
    j failed
.section .data
.balign 64
source:
    .zero 128
.balign 64
target:
    .zero 128
