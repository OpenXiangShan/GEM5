/* Self-checking Decode fusion workloads; assemble one TEST_* variant. */
.option norvc
.option norelax
.section .text
.global _start
_start:
    la t0, trap
    csrw mtvec, t0
    .ifdef TEST_CONTROL
    j control_test
    .else
    .ifdef TEST_VECTOR
    j vector_test
    .else
    j scalar_test
    .endif
    .endif

/* Each expansion is an adjacent, non-recursive low32 fusion candidate. */
.macro low32_pair dest, source
    slli \dest, \source, 32
    srli \dest, \dest, 32
.endm

/* Dense blocks exercise opportunities, not fixed Fetch/FTQ boundaries. */
.balign 64
scalar_test:
    li s0, 64
    li s1, -1
    li s2, 1
    slli s2, s2, 32
    addi s2, s2, -1
.balign 64
dense_pairs:
    low32_pair t0, s1
    low32_pair t1, s1
    low32_pair t2, s1
    low32_pair t3, s1
    low32_pair t4, s1
    low32_pair t5, s1
    low32_pair t6, s1
    low32_pair a1, s1
    bne t0, s2, fail
    bne t1, s2, fail
    bne t2, s2, fail
    bne t3, s2, fail
    bne t4, s2, fail
    bne t5, s2, fail
    bne t6, s2, fail
    bne a1, s2, fail
    addi s0, s0, -1
    bnez s0, dense_pairs

    /* The same candidate with a 2-byte first and second instruction. */
    li s0, -1
    .option push
    .option rvc
compressed_pair:
    c.slli s0, 32
    c.srli s0, 32
    .option pop
    bne s0, s2, fail

    /* Exercise the lui/addi rule independently of the low32 candidates. */
    lui t0, 0x12345
    addi t0, t0, 0x678
    li t1, 0x12345678
    bne t0, t1, fail

    /* Dependent divides and loads provide backend pressure opportunities. */
    li s4, 16
    li s5, 0x123456789abcdef
    li s6, 1
    li s7, 0x80010000
    sd s7, 0(s7)
    fence rw, rw
pressure_loop:
    .rept 16
    divu s5, s5, s6
    .endr
    .rept 8
    low32_pair t0, s1
    low32_pair t1, s1
    low32_pair t2, s1
    low32_pair t3, s1
    .endr
    mv t4, s7
    .rept 16
    ld t4, 0(t4)
    .endr
    bne t4, s7, fail
    bne t0, s2, fail
    bne t1, s2, fail
    bne t2, s2, fail
    bne t3, s2, fail
    addi s4, s4, -1
    bnez s4, pressure_loop
    li t0, 0x123456789abcdef
    bne s5, t0, fail
    j pass

.balign 64
control_test:
    li s0, 64
    li s1, 0
    li s2, 0x80010000
    sd zero, 0(s2)
    li s3, -1
    li s4, 1
    slli s4, s4, 32
    addi s4, s4, -1
control_loop:
    andi t0, s0, 1
    beqz t0, even_path
    low32_pair t1, s3
    bne t1, s4, fail
    addi s1, s1, 1
    j join_paths
    sd s3, 0(s2)
even_path:
    low32_pair t2, s3
    bne t2, s4, fail
    addi s1, s1, 2
join_paths:
    jal ra, return_target
    low32_pair t3, s3
    bne t3, s4, fail
    addi s0, s0, -1
    bnez s0, control_loop
    li t0, 96
    bne s1, t0, fail
    fence rw, rw
    ld t0, 0(s2)
    bnez t0, fail

    /* A reserved OP-IMM-32 encoding must trap exactly once, then resume. */
    li s7, 0
    li s8, 0
illegal_pc:
    .word 0x0000301b
    addi s8, s8, 1
    li t0, 1
    bne s7, t0, fail
    bne s8, t0, fail
    low32_pair t0, s3
    bne t0, s4, fail
    j pass

return_target:
    low32_pair t4, s3
    bne t4, s4, fail
    ret
    sd s3, 0(s2)

.balign 64
vector_test:
    /* Enable vector state while retaining all other mstatus fields. */
    li t0, 0x600
    csrs mstatus, t0
    li s0, 32
    li s1, -1
    li s2, 1
    slli s2, s2, 32
    addi s2, s2, -1
vector_loop:
    li t0, 2
    vsetvli t1, t0, e64, m1, ta, ma
    bne t1, t0, fail
    vmv.v.i v1, 7
    low32_pair t2, s1
    vadd.vv v2, v1, v1
    low32_pair t3, s1
    vmv.x.s t4, v2
    li t5, 14
    bne t4, t5, fail
    bne t2, s2, fail
    bne t3, s2, fail
    /* Check all active lanes, not only the first extracted element. */
    li t0, 0x80010000
    vse64.v v2, (t0)
    fence rw, rw
    li t1, 2
vector_verify:
    ld t2, 0(t0)
    bne t2, t5, fail
    addi t0, t0, 8
    addi t1, t1, -1
    bnez t1, vector_verify
    addi s0, s0, -1
    bnez s0, vector_loop
    j pass

.balign 4
trap:
    .ifdef TEST_CONTROL
    csrr t0, mcause
    li t1, 2
    bne t0, t1, fail
    csrr t0, mepc
    la t1, illegal_pc
    bne t0, t1, fail
    addi s7, s7, 1
    addi t0, t0, 4
    csrw mepc, t0
    mret
    .else
    j fail
    .endif

pass:
    li a0, 0
    .word 0x0000006b
fail:
    /* A failed check reaches the runner's instruction limit, never pass. */
    j fail
