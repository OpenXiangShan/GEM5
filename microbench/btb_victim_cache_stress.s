# BTB Victim Cache Stress Test (32B block with >4 branches)
# Goal: Place 5 branches within one 32-byte block to overflow a 4-way set,
#       so evictions happen and victim cache can help.

.section .text
.global _start

_start:
    # Preload constants 0..4 into s0..s4 for branch comparisons
    li s0, 0
    li s1, 1
    li s2, 2
    li s3, 3
    li s4, 4

    # Iteration counter: large outer loop to exercise BTB/VC behavior
    li t0, 100000           # iterations

outer_loop:
    # Create an index in [0..7] to rotate which branch becomes taken
    andi t1, t0, 7

    # Ensure hot basic block starts at a 32-byte boundary
    .p2align 5              # 2^5 = 32 bytes alignment

hot_block:
    # Exactly 8 instructions (8 x 4B = 32B). 5 branches + 3 nops.
    # In each iteration, at most one beq is taken; across iterations
    # all 5 are taken at different times to fill >4 entries in one set.
    beq t1, s0, taken_path  # 1st branch in 32B block
    beq t1, s1, taken_path  # 2nd branch in 32B block
    beq t1, s2, taken_path  # 3rd branch in 32B block
    beq t1, s3, taken_path  # 4th branch in 32B block
    beq t1, s4, taken_path  # 5th branch in 32B block (exceeds 4-way set)
    nop                     # padding to keep within 32B
    nop
    nop

fallthrough:
    # None of the branches taken: loop control
    addi t0, t0, -1
    bnez t0, outer_loop
    j exit

taken_path:
    # Taken branch target (placed outside the 32B block)
    addi t0, t0, -1
    bnez t0, outer_loop

exit:
    li a0, 0
    li a7, 93
    ecall


