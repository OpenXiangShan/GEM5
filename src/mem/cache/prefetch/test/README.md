bop test usage:
```bash
scons build/RISCV/mem/cache/prefetch/test/bop.test.debug -j64
# run all
./build/RISCV/mem/cache/prefetch/test/bop.test.debug
# run single test
./build/RISCV/mem/cache/prefetch/test/bop.test.debug --gtest_filter=BOPTest.LearnAndPrefetch
```

cmc test usage:
```bash
scons build/RISCV/mem/cache/prefetch/test/cmc.test.debug -j64
# run all
./build/RISCV/mem/cache/prefetch/test/cmc.test.debug
```