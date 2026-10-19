/* Exercise the existing hlvx.hu decoder with HS-mode G-stage translation. */
.option norvc
.section .text
.global _start
_start:
    la t0, fail
    csrw mtvec, t0
    csrw medeleg, zero
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x0f
    csrw pmpcfg0, t0

    /* Sv48x4 root entry 0 points to a level-2 identity gigapage table. */
    la t0, root_table
    la t1, level2_table
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, 1
    sd t1, 0(t0)
    la t2, host_root
    la t3, host_level2
    srli t3, t3, 12
    slli t3, t3, 10
    ori t3, t3, 1
    sd t3, 0(t2)
    fence rw, rw
    srli t0, t0, 12
    li t1, 0x9000000000000000
    or t0, t0, t1
    csrw 0x680, t0 /* hgatp: Sv48x4 */
    /* HS and VS use the same identity mapping, with U clear for S accesses. */
    srli t2, t2, 12
    or t2, t2, t1
    csrw satp, t2
    csrw 0x280, t2 /* vsatp: Sv48 */
    li t0, 0x100
    csrs 0x600, t0 /* hstatus.SPVP = S */
    la t0, hs_mode
    csrw mepc, t0
    li t0, 0x800
    csrw mstatus, t0
    mret
hs_mode:
    la a1, halfword
    li a0, 0x777
    .word 0x6435c573 /* hlvx.hu a0, (a1) */
    li t0, 0xbeef
    bne a0, t0, fail
    li a0, 0
    .word 0x0000006b
fail:
    j fail

.section .data
.balign 8
halfword:
    .hword 0xbeef
.balign 16384
root_table:
    .zero 16384
.balign 4096
level2_table:
    .zero 16
    /* GPA 0x80000000..0xbfffffff maps identically; V/R/W/X/U/A/D set. */
    .dword 0x200000df
    .zero 4072
.balign 4096
host_root:
    .zero 4096
host_level2:
    .zero 16
    /* HS identity gigapage: V/R/W/X/A/D set, U clear. */
    .dword 0x200000cf
    .zero 4072
