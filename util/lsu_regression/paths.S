/* Bare-metal path tests. CASE selects one independent scenario. */
.option nopic
.option norvc
.section .text.init
.global _start, target_begin, target_end, trap_handler, success_exit, failed
_start:
    la t0, trap_handler
    csrw mtvec, t0
    la s0, data
    li s1, 0
    li s2, 0
    li s3, 0
    li s4, 0
    li s5, 0
    li s6, 0
    li s7, 0
    li s8, 0
    li s9, 0
    li s10, 0
    li s11, 0
    li t1, 0x1122334455667788
 target_begin:
#if CASE == 0
    /* Byte merging and overlapping writes; final little-endian value. */
    sd t1, 0(s0)
    li t2, 0xaa
    sb t2, 1(s0)
    li t2, 0xbbcc
    sh t2, 2(s0)
    li t2, 0xddeeff00
    sw t2, 4(s0)
    ld t3, 0(s0)
    li t4, 0xddeeff00bbccaa88
#if EXPECT_BAD
    xori t4, t4, 1
#endif
    bne t3, t4, failed
#elif CASE == 1
    /* Young partial store must not be bypassed for an older full store. */
    sd t1, 0(s0)
    li t2, 0xaa
    sb t2, 0(s0)
    ld t3, 0(s0)
    li t4, 0x11223344556677aa
    bne t3, t4, failed
#elif CASE == 2
    /* An architectural alignment fault, then return to the next instruction. */
    ld t3, 1(s0)
    li t0, 1
    bne s1, t0, failed
    li t0, 4
    bne s2, t0, failed
    addi t0, s0, 1
    bne s3, t0, failed
    la t0, target_begin
    bne s4, t0, failed
    sd t1, 0(s0)
misaligned_store_pc:
    sd t1, 1(s0)
    li t0, 2
    bne s1, t0, failed
    li t0, 6
    bne s2, t0, failed
    addi t0, s0, 1
    bne s3, t0, failed
    la t0, misaligned_store_pc
    bne s4, t0, failed
#elif CASE == 3
    /* Reservation result and AMO old/new values. */
    sd t1, 0(s0)
    fence rw,rw
    li t6, 8
lrsc_retry:
    lr.d t2, (s0)
    bne t2, t1, failed
    li t3, 9
    sc.d t4, t3, (s0)
    beqz t4, lrsc_success
    addi t6, t6, -1
    bnez t6, lrsc_retry
    j failed
lrsc_success:
    addi t0, s0, 16
    lr.d t2, (t0)
    li t3, 7
    sc.d t4, t3, (s0)
    beqz t4, failed
    li t3, 4
    amoadd.d.aqrl t4, t3, (s0)
    li t5, 9
    bne t4, t5, failed
    ld t4, 0(s0)
    li t5, 13
    bne t4, t5, failed
#elif CASE == 4
    /* Stores younger than a data-dependent taken branch are wrong path. */
    li t0, 4096
    add t0, s0, t0
    ld t2, 0(t0)
    beqz t2, correct_path
    sd t1, 0(s0)
    ld t3, 1(s0)
    j failed
correct_path:
    fence rw,rw
    ld t3, 0(s0)
    bnez t3, failed
    bnez s1, failed
#elif CASE == 5
    /* Independent stores behind an old cache-miss-dependent data producer. */
    li t0, 4096
    add t0, s0, t0
    ld t2, 0(t0)
    sd t2, 0(s0)
    .set off, 64
    .rept 48
    sd t1, off(s0)
    .set off, off+8
    .endr
    fence rw,rw
    .set off, 64
    .rept 48
    ld t2, off(s0)
    bne t2, t1, failed
    .set off, off+8
    .endr
#elif CASE == 6
    /* Same-line updates interleaved with cold lines to exercise frozen versions. */
    li t6, 24
loop:
    sd t6, 0(s0)
    sd t6, 64(s0)
    sd t6, 128(s0)
    sd t6, 192(s0)
    addi t6, t6, -1
    bnez t6, loop
    fence rw,rw
    ld t2, 0(s0)
    li t3, 1
    bne t2, t3, failed
    ld t2, 192(s0)
    bne t2, t3, failed
#elif CASE == 7
    /* Sv39 1GiB read-only leaf: virtual 0x40000000 -> physical 0x80000000. */
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 15
    csrw pmpcfg0, t0
    la t0, root_pt
    li t2, 0x2000004b
    sd t2, 8(t0)
    fence rw,rw
    srli t0, t0, 12
    li t2, 0x8000000000000000
    or t0, t0, t2
    csrw satp, t0
    sfence.vma zero,zero
    li t0, 0x1800
    csrc mstatus, t0
    li t0, 0x20800
    csrs mstatus, t0
    li t5, 0x40001040
translated_load:
    ld t3, 0(t5)
    li t0, 0x20000
    csrc mstatus, t0
    bnez t3, failed
    li t0, 0x1800
    csrc mstatus, t0
    li t0, 0x20800
    csrs mstatus, t0
permission_store:
    sd t1, 0(t5)
    li t0, 1
    bne s1, t0, failed
    li t0, 15
    bne s2, t0, failed
    bne s3, t5, failed
    la t0, permission_store
    bne s4, t0, failed
    li t0, 0x1800
    csrc mstatus, t0
    li t0, 0x20800
    csrs mstatus, t0
unmapped_load:
    ld t3, 0(zero)
    li t0, 2
    bne s1, t0, failed
    li t0, 13
    bne s2, t0, failed
    bnez s3, failed
    la t0, unmapped_load
    bne s4, t0, failed
    csrw satp, zero
    sfence.vma zero,zero
    ld t3, 0(s0)
    bnez t3, failed
#elif CASE == 9 || CASE == 10
    /* Repeated same-PC unknown-address aliasing trains replay MDP. */
    li s6, 64
    li s7, 3
    li s8, 4096
    add s8, s0, s8
mdp_loop:
    ld t0, 0(s8)
#if CASE == 9
    divu t0, t0, s7
    divu t0, t0, s7
    divu t0, t0, s7
    divu t0, t0, s7
#endif
    add t0, s0, t0
    sd s6, 0(t0)
    ld t2, 0(s0)
    bne t2, s6, failed
    addi s6, s6, -1
    bnez s6, mdp_loop
#elif CASE == 8
    /* A real CLINT software interrupt, acknowledged by the guest handler. */
    li t0, 8
    csrs mie, t0
    csrs mstatus, t0
    li t5, 0x38000000
    li t0, 1
    sw t0, 0(t5)
wait_irq:
    beqz s1, wait_irq
    li t0, 1
    bne s1, t0, failed
    li t0, 0x8000000000000003
    bne s2, t0, failed
    sd t1, 0(s0)
#endif
 target_end:
    fence rw,rw
#if CASE == 0 || CASE == 1
    ld t0, 0(s0)
    bne t0, t4, failed
#elif CASE == 2
    ld t0, 0(s0)
    bne t0, t1, failed
    lbu t0, 8(s0)
    bnez t0, failed
#endif
    ld t0, -8(s0)
    li t2, 0x55aa55aa55aa55aa
    bne t0, t2, failed
success_exit:
    li a0, 0
    .word 0x0000006b
    j failed
trap_handler:
    addi s1, s1, 1
    csrr s2, mcause
    csrr s3, mtval
    csrr s4, mepc
#if CASE == 8
    li t0, 0x38000000
    sw zero, 0(t0)
    mret
#elif CASE == 2 || CASE == 7
    li t0, 0x20000
    csrc mstatus, t0
    addi t0, s4, 4
    csrw mepc, t0
    mret
#else
    j failed
#endif
failed:
    li a0, 1
1:  j 1b
.section .data
.balign 64
    .zero 56
    .quad 0x55aa55aa55aa55aa
.global data
data:
    .zero 8192

.balign 4096
root_pt:
    .zero 4096
