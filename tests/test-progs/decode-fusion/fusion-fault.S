/* Optional self-check of gem5's fused-fault ReExec path.
 * The pinned NEMU disagrees on this reserved C.LUI encoding; run with
 * --disable-difftest and do not count this as a difftest pass.
 */
.option norvc
.option norelax
.section .text
.global _start
_start:
    la t0, trap
    csrw mtvec, t0
    li s0, 0
    li t0, 7
    j fault_pair
.balign 64
fault_pair:
    .2byte 0x6281
    addi t0, t0, 1
    li t1, 8
    bne t0, t1, fail
    li t1, 1
    bne s0, t1, fail
    li a0, 0
    .word 0x0000006b
fail:
    j fail
.balign 4, 0
trap:
    csrr t1, mcause
    li t2, 2
    bne t1, t2, fail
    csrr t1, mepc
    la t2, fault_pair
    bne t1, t2, fail
    addi s0, s0, 1
    li t2, 1
    bne s0, t2, fail
    addi t1, t1, 2
    csrw mepc, t1
    mret
