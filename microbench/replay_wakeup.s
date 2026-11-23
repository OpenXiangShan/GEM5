# Run with make -C microbench run-replay_wakeup.
# Trace ma_store/ma_load and ff_store/ff_load with IQ,Schedule,StorePipeline.
# A waiting load can retry one cycle after STA admission or on STD admission.
.option norvc
.option norelax
.option nopic
.text
.global _start
_start:
    la s0, slots
    li s1, 13
    li s2, 64

ma_loop:
    # Delay the store address; its low three bits always resolve to zero.
    slli t0, s2, 3
    .rept 6
    mul t0, t0, s1
    .endr
    andi t0, t0, 7
    add t0, s0, t0

    # The second store at this PC trains strict wait with a one-slot LFST.
    li t2, 1
    li t3, 52
    bleu s2, t3, ma_store
    li t2, 2
ma_store:
    sd s2, 0(t0)
    addi t2, t2, -1
    addi t0, s0, 8
    bnez t2, ma_store
ma_load:
    ld t1, 0(s0)
    bne t1, s2, fail
    addi s2, s2, -1
    bnez s2, ma_loop
    fence rw, rw

    li s2, 64
ff_loop:
    # Delay only the data. The shorter chain targets same-cycle C_FF enqueue.
    mul t0, s2, s1
    mul t0, t0, s1
    li t3, 32
    bleu s2, t3, ff_store
    .rept 4
    mul t0, t0, s1
    .endr
ff_store:
    sd t0, 64(s0)
ff_load:
    ld t1, 64(s0)
    bne t1, t0, fail
    addi s2, s2, -1
    bnez s2, ff_loop

    li a0, 0
    j exit
fail:
    li a0, 1
exit:
    li a7, 93
    ecall

.data
.balign 64
slots:
    .zero 128
