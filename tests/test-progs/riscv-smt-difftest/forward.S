/* Two-hart scalar load difftest regression. From this directory:
 * riscv64-linux-gnu-gcc -nostdlib -static -fno-pic -fno-pie -mno-relax \
 *     -march=rv64ima_zicsr -mabi=lp64 -Wl,--build-id=none \
 *     -T ../riscv-smt-rar/link.ld forward.S -o forward.elf
 * riscv64-linux-gnu-objcopy -O binary forward.elf forward.bin
 * Run forward.bin with smt_idealkmhv3.py, --raw-cpt, and the multi NEMU REF.
 * SCALAR_LOAD_PASS marks success.
 */
.section .text.init
.globl _start
_start:
    csrr a0, mhartid
    la s0, shared
    la s1, ready
    bnez a0, writer
wait:
    ld t0, 0(s1)
    beqz t0, wait
    fence r,rw
    li t0, 1
    li t1, 2
    /* The newest of two ordinary stores must supply the full word. */
    sd t0, 64(s0)
    sd t1, 64(s0)
.globl full_load
full_load:
    ld a3, 64(s0)
    bne a3, t1, fail
    /* Leave one byte buffered while the remaining bytes come from hart 1. */
    li t2, 0x77
    sb t2, 1(s0)
    mv t0, s0
    li t1, 1
    .rept 5
    divu t0, t0, t1
    .endr
.globl partial_load
partial_load:
    ld a4, 0(t0)
    li t1, 0x8070605040307710
    bne a4, t1, fail
    li a1, 0
    la a0, pass
    j done
writer:
    li t0, 0x8070605040302010
    sd t0, 0(s0)
    fence rw,w
    li t0, 1
    sd t0, 0(s1)
park:
    j park
fail:
    li a1, 2
    la a0, failed
done:
    li t0, 0x40600004
1:
    lbu t1, 0(a0)
    beqz t1, 2f
    sb t1, 0(t0)
    addi a0, a0, 1
    j 1b
2:
    fence rw,rw
    mv a0, a1
    .word 0x0000006b
.section .rodata
pass: .asciz "SCALAR_LOAD_PASS\n"
failed: .asciz "SCALAR_LOAD_FAIL\n"
.section .data
.balign 64
shared: .zero 64
    .dword 3
ready: .dword 0
