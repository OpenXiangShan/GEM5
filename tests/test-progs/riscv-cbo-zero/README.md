# cbo.zero functional regression

The bare-metal program initializes three adjacent 64-byte blocks, clears the
middle block using an unaligned address, and immediately verifies every word.
The neighboring blocks must retain their data. Build with `TEST_S` for S-mode;
add `TEST_DISABLED` to verify that clearing CBZE causes an illegal-instruction
trap without modifying memory. Failures loop until `--maxinsts` is reached;
only success executes the XiangShan m5-exit instruction.

Example (run from the repository root):

```sh
riscv64-linux-gnu-as -march=rv64gc --defsym TEST_S=1 \
  -o /tmp/cbo-zero.o tests/test-progs/riscv-cbo-zero/zero.S
riscv64-linux-gnu-ld -Ttext=0x80000000 -o /tmp/cbo-zero.elf /tmp/cbo-zero.o
riscv64-linux-gnu-objcopy -O binary /tmp/cbo-zero.elf /tmp/cbo-zero.bin
GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/releases/d30fff1ece9e-gem5-r3/normal/riscv64-nemu-interpreter-so \
  build/RISCV/gem5.opt --outdir=/tmp/cbo-zero-out \
  configs/example/idealkmhv3.py --raw-cpt \
  --generic-rv-cpt=/tmp/cbo-zero.bin --maxinsts=10000 > /tmp/cbo-zero.log 2>&1
grep -q 'because m5_exit instruction encountered' /tmp/cbo-zero.log
```

Run all three variants: no definitions, `TEST_S=1`, and both `TEST_S=1` and
`TEST_DISABLED=1`. Keep difftest enabled and use a NEMU REF with `CONFIG_RV_CBO=y`.
These tests cover zeroing, store-to-load visibility, and M/S privilege checks;
they do not establish timing equivalence to hardware or full H-extension coverage.
