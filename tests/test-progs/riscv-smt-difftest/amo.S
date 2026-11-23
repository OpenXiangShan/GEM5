/* Build with the same RV64IMA toolchain and link.ld as forward.S.
 * Define CAS to include AMOCAS.Q success and failure on a Zacas REF.
 * Run with smt_idealkmhv3.py and a multi-hart NEMU REF.
 */
.section .text.init
.globl _start
_start:
    csrr t0, mhartid
    la s0, data
    la s1, ready
    bnez t0, writer
wait:
    ld t0, 0(s1)
    beqz t0, wait
    fence r,rw
    li t0, 1
.globl checked_add
checked_add:
    amoadd.d.aqrl a0, t0, (s0)
    li t1, 5
    bne a0, t1, fail
    ld t1, 0(s0)
    li t2, 6
    bne t1, t2, fail
    amoadd.w zero, t0, (s0)
    ld t1, 0(s0)
    li t2, 7
    bne t1, t2, fail
#ifdef CAS
    addi s0, s0, 16
    li a0, 11
    li a1, 12
    li a2, 13
    li a3, 14
    /* amocas.q a0, a2, (s0), compare succeeds. */
    .insn r 0x2f, 4, 0x14, a0, s0, a2
    li t1, 11
    bne a0, t1, fail
    li t1, 12
    bne a1, t1, fail
    li a2, 21
    li a3, 22
    /* Same comparison now fails. Both halves must stay intact. */
    .insn r 0x2f, 4, 0x14, a0, s0, a2
    li t1, 13
    bne a0, t1, fail
    ld t2, 0(s0)
    bne t2, t1, fail
    li t1, 14
    bne a1, t1, fail
    ld t2, 8(s0)
    bne t2, t1, fail
#endif
    la a0, pass
    j done
writer:
    li t0, 5
    sd t0, 0(s0)
    li t0, 11
    sd t0, 16(s0)
    li t0, 12
    sd t0, 24(s0)
    fence rw,w
    li t0, 1
    sd t0, 0(s1)
1:  j 1b
fail:
    la a0, failed
done:
    li t0, 0x40600004
1:  lbu t1, 0(a0)
    beqz t1, 2f
    sb t1, 0(t0)
    addi a0, a0, 1
    j 1b
2:  fence rw,rw
    .word 0x0000006b
.section .rodata
pass: .asciz "AMO_DIFF_PASS\n"
failed: .asciz "AMO_DIFF_FAIL\n"
.section .data
.balign 64
data: .zero 64
ready: .dword 0
