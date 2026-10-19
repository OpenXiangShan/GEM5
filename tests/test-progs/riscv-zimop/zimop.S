/* Exercise every Zimop number without requiring Zimop assembler support. */
.option norvc
.section .text
.global _start

.macro save_regs offset
    .irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd x\reg, (\offset + 8 * \reg)(s0)
    .endr
.endm

.macro prepare rd, rs1, rs2=0
    /* s0 addresses the snapshots and must not be a destination. */
    .if \rd == 8
    .error "s0 is reserved for the register snapshots"
    .endif
    .if \rd != 0
    li x\rd, 0x12345
    .endif
    .if \rs1 != 0
    li x\rs1, -17
    .endif
    .if \rs2 != 0
    li x\rs2, 0x76543
    .endif
    li t6, 1
    sd t6, 512(s0)
    save_regs 0
.endm

.macro check_result rd
    /* The marker store must execute immediately after the four-byte MOP. */
    ld t0, 512(s0)
    bnez t0, fail
    mv t0, s0
    li t1, 0
    li t4, \rd
    li t5, 32
check_regs_\@:
    beq t1, t4, check_zero_\@
    ld t2, 0(t0)
    j compare_\@
check_zero_\@:
    li t2, 0
compare_\@:
    ld t3, 256(t0)
    bne t2, t3, fail
    addi t0, t0, 8
    addi t1, t1, 1
    bne t1, t5, check_regs_\@
    addi t0, s0, 520
    li t1, 8
    li t2, 0x5aa55aa5a55aa55a
check_memory_\@:
    ld t3, 0(t0)
    bne t2, t3, fail
    addi t0, t0, 8
    addi t1, t1, -1
    bnez t1, check_memory_\@
.endm

.macro case_r n, rd, rs1
    prepare \rd, \rs1
    .word (0x81c04073 | ((\n & 16) << 26) | ((\n & 12) << 24) | ((\n & 3) << 20) | (\rs1 << 15) | (\rd << 7))
    sd zero, 512(s0)
    save_regs 256
    check_result \rd
.endm

.macro case_rr n, rd, rs1, rs2
    prepare \rd, \rs1, \rs2
    .word (0x82004073 | ((\n & 4) << 28) | ((\n & 3) << 26) | (\rs2 << 20) | (\rs1 << 15) | (\rd << 7))
    sd zero, 512(s0)
    save_regs 256
    check_result \rd
.endm

_start:
    .ifdef TEST_FAIL
    j fail
    .endif
    la t0, fail
    csrw mtvec, t0
    csrw medeleg, zero
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x0f
    csrw pmpcfg0, t0
    .ifdef TEST_MODE
    la t0, test_body
    csrw mepc, t0
    li t0, (TEST_MODE << 11)
    csrw mstatus, t0
    mret
    .endif

test_body:
    la s0, snapshots
    /* Include a MOP at a halfword-aligned PC, as in the original vDSO. */
    .2byte 0x0001
    case_r 28, 10, 10
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    case_r \n, 5, 6
    case_r \n, 0, 11
    case_r \n, 10, 10
    case_r \n, 19, 0
    case_r \n, 2, 31
    .endr
    .irp n, 0,1,2,3,4,5,6,7
    case_rr \n, 5, 6, 7
    case_rr \n, 0, 10, 11
    case_rr \n, 10, 10, 11
    case_rr \n, 11, 10, 11
    case_rr \n, 12, 12, 12
    case_rr \n, 18, 0, 0
    case_rr \n, 19, 7, 7
    /* n=7 emits the original 0xce104073 encoding. */
    case_rr \n, 0, 0, 1
    .endr
    li a0, 0
    .word 0x0000006b
.balign 4
fail:
    j fail

.section .data
.balign 8
snapshots:
    .zero 512
    .dword 1
    .rept 8
    .dword 0x5aa55aa5a55aa55a
    .endr
