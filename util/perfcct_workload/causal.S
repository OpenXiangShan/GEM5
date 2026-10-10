/* Deterministic RV64 SE probes. No runtime or libc is required. */
    .option norvc
    .option norelax
    .section .text
    .globl _start
_start:
    la s0, slot
    ld t2, 0(s0)
    fence rw, rw
    li t0, 3
    li t1, 0x123456789abcdef
    li s1, 16
    .globl stlf_probe
stlf_probe:
    mv a0, t1
    /* The store address is ready; its data waits for this divide chain. */
    .rept 12
    divu a0, a0, t0
    xor a0, a0, t1
    .endr
    sd a0, 0(s0)
    ld a1, 0(s0)
    li a2, 0x14c14f1863d14ba
    bne a1, a2, fail_stlf
    addi s1, s1, -1
    bnez s1, stlf_probe

    /* Cold, page-strided pointer chasing followed by independent work. */
    la s0, chain
    li s1, 127
    li s2, 0
    .globl cold_load_probe
cold_load_probe:
    ld s0, 0(s0)
    .rept 8
    addi s2, s2, 1
    .endr
    addi s1, s1, -1
    bnez s1, cold_load_probe
    la t2, chain_end
    bne s0, t2, fail_chain
    li t2, 1016
    bne s2, t2, fail_chain

    /* An independent ready instruction preceding a delayed group member.
     * Actual grouping is model/configuration dependent and must be traced. */
    li s1, 32
    .globl group_ready_probe
    .balign 32
group_ready_probe:
    addi s2, zero, 7
    divu a0, t1, t0
    addi s3, zero, 9
    xor a0, a0, t1
    addi s1, s1, -1
    bnez s1, group_ready_probe
    li t2, 0x14252450a92894a
    bne a0, t2, fail_group
    li a0, 0
    j exit
fail_stlf:
    li a0, 1
    j exit
fail_chain:
    li a0, 2
    j exit
fail_group:
    li a0, 3
exit:
    li a7, 93
    ecall
    unimp

    .section .data
    .balign 64
slot:
    .quad 0
    .balign 4096
chain:
    .rept 127
    .quad . + 4096
    .zero 4088
    .endr
chain_end:
    .quad 0
    .section .note.GNU-stack,"",@progbits
