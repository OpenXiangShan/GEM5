/* Execute permission, canonical-address and demanded-byte fetch regressions. */
.option norelax
.section .text
.global _start
_start:
    csrw mie, zero
    csrw medeleg, zero
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x0f
    csrw pmpcfg0, t0
    la t0, trap
    csrw mtvec, t0

    /* Identity-map the code region with a 1GiB executable leaf. */
    la t0, root_table
    .ifdef SV48
    la t1, level2_table
    srli t2, t1, 12
    slli t2, t2, 10
    ori t2, t2, 1
    sd t2, 0(t0)
    mv t0, t1
    .endif
    li t1, 0x200000cf
    sd t1, 16(t0)
    la t1, level1_table
    srli t2, t1, 12
    slli t2, t2, 10
    ori t2, t2, 1
    sd t2, 0(t0)
    la t2, level0_table
    srli t3, t2, 12
    slli t3, t3, 10
    ori t3, t3, 1
    sd t3, 0(t1)

    /* 0x1000 is X-only. Its final halfword is a compressed return. */
    la t0, execute_page
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x49
    sd t0, 8(t2)
    /* 0x3000 is readable/writable but not executable. */
    la t0, execute_page
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0xc7
    sd t0, 24(t2)
    /* A 32-bit instruction at 0x5ffe needs unmapped bytes at 0x6000. */
    la t0, crossing_page
    srli t0, t0, 12
    slli t0, t0, 10
    ori t0, t0, 0x49
    sd t0, 40(t2)
    fence rw, rw

    la t0, root_table
    srli t0, t0, 12
    .ifdef SV48
    li t1, 0x9000000000000000
    .else
    li t1, 0x8000000000000000
    .endif
    or t0, t0, t1
    csrw satp, t0
    sfence.vma zero, zero
    la t0, smode
    csrw mepc, t0
    li t0, 0x800
    csrw mstatus, t0
    mret

smode:
    li s1, 0
    li s0, 64
calls:
    li t0, 0x1000
    jalr ra, t0, 0
    addi s0, s0, -1
    bnez s0, calls
    li t0, 64
    bne s1, t0, fail

    /* The high address aliases the low VPN but is not sign-extended. */
    li s4, 12
    .ifdef SV48
    li s5, 0x1000000001000
    .else
    li s5, 0x8000001000
    .endif
    mv s6, s5
    la s7, after_canonical
    jr s5
    j fail
after_canonical:
    li s5, 0x3000
    mv s6, s5
    la s7, after_nx
nx_site:
    jr s5
    j fail
after_nx:
    /* Reuse the trained indirect branch, now with an executable target.
     * Any speculative NX fault must be discarded by the older redirect. */
    li s5, 0x1000
    la ra, after_wrongpath_nx
    j nx_site
after_wrongpath_nx:
    li t0, 65
    bne s1, t0, fail
    .ifndef SKIP_BOUNDARY
    li t0, 0x1ffe
    jalr ra, t0, 0
    /* A compressed return must not translate unmapped 0x2000. */
    li s5, 0x6000
    li s6, 0x5ffe
    la s7, after_cross
    jr s6
    j fail
after_cross:
    .endif
    li a0, 0
    .word 0x0000006b
fail:
    j fail

.balign 4
trap:
    csrr t0, mcause
    bne t0, s4, fail
    csrr t0, mtval
    bne t0, s5, fail
    csrr t0, mepc
    bne t0, s6, fail
    csrw mepc, s7
    mret

.balign 4096
execute_page:
    addi s1, s1, 1
    ret
    .space 4094 - (. - execute_page)
    .2byte 0x8082
.balign 4096
crossing_page:
    .space 4094
    .2byte 0x0013
.balign 4096
root_table:
    .zero 4096
level2_table:
    .zero 4096
level1_table:
    .zero 4096
level0_table:
    .zero 4096
