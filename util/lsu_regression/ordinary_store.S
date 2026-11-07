/* One ordinary scalar store, with byte and adjacent-guard checks.
 * WARM=1 adds a read before the target store; no other stores are executed.
 * Only the success path executes the gem5 custom exit instruction.
 */
#ifndef WARM
#define WARM 0
#endif
.option nopic
.option norvc
.section .text.init
.global _start
.global target_store
.global check_before_fence
.global check_after_fence
.global success_exit
.global failed
_start:
    la t0, failed
    csrw mtvec, t0
    la s0, target
    li s1, 0x1122334455667788
#if WARM
    ld t0, 0(s0)
    bnez t0, failed
    fence rw, rw
#endif
.balign 4
target_store:
    sd s1, 0(s0)
check_before_fence:
    ld t0, 0(s0)
    bne t0, s1, failed
    fence rw, rw
check_after_fence:
#if EXPECT_BAD
    xori s1, s1, 1
#endif
    ld t0, 0(s0)
    bne t0, s1, failed
    li t1, 0
    mv t2, s1
1:
    add t3, s0, t1
    lbu t4, 0(t3)
    andi t5, t2, 255
    bne t4, t5, failed
    srli t2, t2, 8
    addi t1, t1, 1
    li t6, 8
    bltu t1, t6, 1b
    ld t0, -8(s0)
    li t1, 0x8877665544332211
    bne t0, t1, failed
    ld t0, 8(s0)
    li t1, 0xaabbccddeeff0011
    bne t0, t1, failed
success_exit:
    li a0, 0
    .word 0x0000006b
2:
    j 2b
.balign 4
failed:
    csrr s2, mcause
    csrr s3, mepc
    csrr s4, mtval
    li a0, 1
3:
    j 3b
.section .data
.balign 64
.global guard_before
.global target
.global guard_after
guard_before:
    .quad 0x8877665544332211
target:
    .quad 0
guard_after:
    .quad 0xaabbccddeeff0011
    .zero 40
