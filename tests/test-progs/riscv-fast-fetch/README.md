# Fast-fetch block boundaries

This bare-metal RISC-V program repeatedly executes blocks containing 7, 8, 9,
12 and 16 instructions. It combines compressed instructions with a block of
32-bit instructions and a data-dependent branch before the block chain.
The exit instruction is the XiangShan/NEMU simulator exit encoding.
Assemble with `-Wa,--defsym,TEST_TRAP=1` to add an illegal compressed instruction
and a returning machine-mode trap handler on every iteration.

Build outside the source directory:

```sh
riscv64-linux-gnu-gcc -nostdlib -static -march=rv64gc -mabi=lp64d \
    -Wl,-Ttext=0x80000000,--no-relax -o /tmp/fast-fetch-blocks.elf \
    tests/test-progs/riscv-fast-fetch/blocks.S
riscv64-linux-gnu-objcopy -O binary /tmp/fast-fetch-blocks.elf \
    /tmp/fast-fetch-blocks.bin
```

Run the image with KMHV3, `--raw-cpt`, `--generic-rv-cpt`, and NEMU difftest.
Enable `FastFetch` tracing and validate the resulting trace with
`util/xs_scripts/check_fast_fetch_routing.py`. Compare mask routing, legacy
continuous fast fetch, and fast disabled on the same image. The actual FTQ
boundaries depend on prediction state; inspect FTQ IDs and PCs in the trace
rather than assuming every source label is already a predicted block.
