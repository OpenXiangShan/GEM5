/* Overlapping stores must merge while the first miss is still outstanding.
 * Check both forwarding before fence and the final cache contents after it.
 * The runner uses a small SBuffer and slow memory to exercise slot reuse.
 */
.section .text
.globl _start
.option norvc
_start:
    li s0, 0x80010000
    li s1, 16
    li s2, 0x1122334455667788
    li s3, 0x11223344ccbb7788
    li s4, 0x76543210abcdef01
    li s5, 0xaabb
    li s6, 0xcc
1:
    sd s2, 0(s0)
    sd s4, 8(s0)
    .rept 128
    nop
    .endr
    sh s5, 2(s0)
    .rept 128
    nop
    .endr
    sb s6, 3(s0)
    sd s4, 16(s0)
    .rept 128
    nop
    .endr
    ld t0, 0(s0)
    bne t0, s3, fail
    ld t0, 8(s0)
    bne t0, s4, fail
    ld t0, 16(s0)
    bne t0, s4, fail
    fence rw, rw
    ld t0, 0(s0)
    bne t0, s3, fail
    ld t0, 8(s0)
    bne t0, s4, fail
    ld t0, 16(s0)
    bne t0, s4, fail
    addi s0, s0, 64
    addi s1, s1, -1
    bnez s1, 1b
    li a0, 0
    .word 0x0000006b
fail:
    /* XS m5_exit ignores a0: a failed check must not reach that opcode. */
    j fail
