/* CoRR: a younger load must not observe an older write than an older load.
 * Each trial has exactly one transition of x from 0 to 1. The reader drains
 * its initialization before publishing go; the writer drains x before done.
 * There is deliberately no fence between the observed loads by default.
 * Prints SMT_RAR_FAIL for (old, young) = (1, 0), otherwise SMT_RAR_PASS.
 *
 * Compile with the adjacent link.ld and -march=rv64ima_zicsr -mabi=lp64.
 * READER_HART selects 0/1; WRITER_DELAY and ADDRESS_DIVS tune the race.
 * ORDERED inserts fence r,r between the observed loads. WRITER_AMO,
 * WRITER_SC and WRITER_FAILED_SC exercise alternative write operations.
 */
#ifndef TRIALS
#define TRIALS 128
#endif
#ifndef WRITER_DELAY
#define WRITER_DELAY 16
#endif
#ifndef ADDRESS_DIVS
#define ADDRESS_DIVS 16
#endif
#ifndef READER_HART
#define READER_HART 0
#endif

.section .text.init
.globl _start
_start:
    csrr a0, mhartid
    li t0, 1
    bgtu a0, t0, park
    la sp, stacks
    addi t0, a0, 1
    slli t0, t0, 12
    add sp, sp, t0
    la t0, trap
    csrw mtvec, t0
    li t0, READER_HART
    beq a0, t0, reader
    j writer

reader:
    la s0, shared_x
    la s1, go
    la s2, done
    li s3, 1
    li s4, TRIALS
    li s5, 0
    li s6, 0
    li s7, 0
    li s8, 1
reader_trial:
    sd zero, 0(s0)
    fence rw, rw
    sd s3, 0(s1)
    fence rw, rw

    /* Delay only the older load's address, not the younger load's address. */
    mv t0, s0
    .rept ADDRESS_DIVS
    divu t0, t0, s8
    .endr
.globl older_load
older_load:
    ld a1, 0(t0)
#ifdef ORDERED
    fence r, r
#endif
.globl younger_load
younger_load:
    ld a2, 0(s0)
    fence rw, rw
    /* Check the outcome only after both register results are available. */
    li t1, 1
    beq a1, t1, older_one
    bnez a1, fail
    beqz a2, both_zero
    bne a2, t1, fail
    addi s6, s6, 1
    j wait_done
both_zero:
    addi s5, s5, 1
    j wait_done
older_one:
    beqz a2, fail
    bne a2, t1, fail
    addi s7, s7, 1
wait_done:
    ld t0, 0(s2)
    bne t0, s3, wait_done
    fence rw, rw
    /* Verify the writer completed the intended transition, independently of
     * whether the observed load pair raced early enough to see it. */
    ld t0, 0(s0)
#ifdef WRITER_FAILED_SC
    bnez t0, fail
#else
    li t1, 1
    bne t0, t1, fail
#endif
    addi s3, s3, 1
    bgeu s4, s3, reader_trial

    la a0, pass_msg
    call puts
    mv a0, s5
    call puthex
    la a0, zero_one_msg
    call puts
    mv a0, s6
    call puthex
    la a0, one_one_msg
    call puts
    mv a0, s7
    call puthex
    li a0, 10
    call putchar
    fence rw, rw
    li a0, 0
    .word 0x0000006b
    j park

writer:
    la s0, shared_x
    la s1, go
    la s2, done
    li s3, 1
    li s4, TRIALS
    li s5, 1
    li s6, 0
writer_wait:
    ld t0, 0(s1)
    bne t0, s3, writer_wait
    fence rw, rw
    mv t1, s5
    .rept WRITER_DELAY
    add t1, t1, s6
    .endr
#ifdef WRITER_AMO
    amoswap.d zero, t1, (s0)
#elif defined(WRITER_SC)
5:
    lr.d t2, (s0)
    sc.d t3, t1, (s0)
    bnez t3, 5b
#elif defined(WRITER_FAILED_SC)
    sc.d t3, t1, (s0)
    beqz t3, trap
#else
    sd t1, 0(s0)
#endif
    fence rw, rw
    sd s3, 0(s2)
    fence rw, rw
    addi s3, s3, 1
    bgeu s4, s3, writer_wait
park:
    j park

fail:
    la a0, fail_msg
    call puts
    mv a0, s3
    call puthex
    la a0, old_msg
    call puts
    mv a0, a1
    call puthex
    la a0, young_msg
    call puts
    mv a0, a2
    call puthex
    li a0, 10
    call putchar
    fence rw, rw
    li a0, 1
    .word 0x0000006b
    j park
trap:
    la a0, trap_msg
    call puts
    csrr a0, mcause
    call puthex
    li a0, 10
    call putchar
    fence rw, rw
    li a0, 2
    .word 0x0000006b
    j park

/* Only the reader prints, so the UART needs no inter-hart locking. */
putchar:
    li t4, 0x40600004
    sb a0, 0(t4)
    ret
puts:
    mv t3, a0
    li t4, 0x40600004
1:
    lbu t5, 0(t3)
    beqz t5, 2f
    sb t5, 0(t4)
    addi t3, t3, 1
    j 1b
2:
    ret
puthex:
    mv t3, a0
    li t4, 0x40600004
    li t5, 60
3:
    srl t6, t3, t5
    andi t6, t6, 15
    sltiu a3, t6, 10
    addi t6, t6, 48
    bnez a3, 4f
    addi t6, t6, 39
4:
    sb t6, 0(t4)
    addi t5, t5, -4
    bgez t5, 3b
    ret

.section .rodata
pass_msg: .asciz "SMT_RAR_PASS 00="
zero_one_msg: .asciz " 01="
one_one_msg: .asciz " 11="
fail_msg: .asciz "SMT_RAR_FAIL trial="
old_msg: .asciz " old="
young_msg: .asciz " young="
trap_msg: .asciz "SMT_RAR_TRAP cause="

.section .data
.balign 64
shared_x: .dword 0
.balign 64
go: .dword 0
.balign 64
done: .dword 0

.section .bss
.balign 16
stacks: .skip 8192
