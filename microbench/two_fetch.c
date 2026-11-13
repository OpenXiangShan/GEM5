// Paired-fetch layout: B0 and B1 share a page, but need not share a line.
// Build with -DBLOCK_OFFSET=136 for distinct banks, or 128 for overlap.
// Each loop must produce the same checksum with all frontend settings.

#ifndef BLOCK_OFFSET
#define BLOCK_OFFSET 136
#endif
#ifndef FIRST_OFFSET
#define FIRST_OFFSET 0
#endif
#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

extern long paired_loop(long count);

__asm__(
    ".text\n"
    ".option push\n"
    ".option norvc\n"
    ".balign 4096\n"
    ".global paired_loop\n"
    "paired_loop:\n"
    "li a1, 0\n"
    "j paired_b0\n"
    ".balign 4096\n"
    ".if " STRINGIFY(FIRST_OFFSET) "\n"
    ".space " STRINGIFY(FIRST_OFFSET) "\n"
    ".endif\n"
    "paired_b0:\n"
    "addi a1, a1, 1\n"
    "j paired_b1\n"
    ".space " STRINGIFY(BLOCK_OFFSET) " - 8\n"
    "paired_b1:\n"
    "addi a0, a0, -1\n"
    "bnez a0, paired_b0\n"
    "mv a0, a1\n"
    "ret\n"
    ".option pop\n"
);

#ifdef FREESTANDING
__asm__(
    ".global _start\n"
    "_start:\n"
    "li a0, 2000\n"
    "call paired_loop\n"
    "li t0, 2000\n"
    "sub a0, a0, t0\n"
    "snez a0, a0\n"
    "li a7, 93\n"
    "ecall\n"
);
#endif

int
main(void)
{
    return paired_loop(20000) == 20000 ? 0 : 1;
}
