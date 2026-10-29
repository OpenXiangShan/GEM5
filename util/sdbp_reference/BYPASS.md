# SDBP L2 refill bypass

The Kunminghu SDBP configuration enables bypass by default. Use
`--no-sdbp-enable-bypass` for the replacement-only control. Other replacement
policies retain their existing behavior. Bypass requires an L3 cache.

A predicted-dead data refill uses the L2 temporary block instead of installing
L2 tags, even if an invalid way is available. Requests without a PC, actual
prefetches, upgrades, writebacks and cache maintenance do not bypass. A bypassed
miss still updates the independent sampler, so the predictor can learn reuse.

The response marks the receiving L1 line. Exclusive L1D fills acquire the dirty
bit; shared lines and L1I lines retain their normal coherence permissions.
Every marked L1 victim produces a data-bearing `WritebackDirty` packet, including
otherwise clean victims. The packet retains the normal sharer information and
traverses the L2 write buffer without allocating L2 tags. If a later request has
installed an L2 copy, the returning victim invalidates that copy before passing
the current data to L3. At L3 the packet follows the ordinary writeback allocation
and update path. Subsequent L3 replacements follow the usual policy.

In this source tree `CleanEvict` has no payload; `WritebackClean` has data.
Using `WritebackDirty` also prevents clean-writeback filtering from discarding
the returning line. The route is local packet/block metadata, not a request flag;
snoop packet copies do not inherit it. Block invalidation clears the marker and
block moves preserve it. Timing and atomic accesses use the same refill and
eviction rules. Existing write-buffer backpressure and arbitration apply.

## Validation

Build the simulator and targeted unit tests:

```sh
scons build/RISCV/gem5.opt \
  build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt \
  build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt \
  --gold-linker -j32
build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt
build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt
```

Build and run the self-checking guest:

```sh
riscv64-linux-gnu-gcc -nostdlib -static -march=rv64im -mabi=lp64 \
  -Wl,--build-id=none -o /tmp/sdbp-traffic tests/gem5/sdbp_bypass/traffic.S
build/RISCV/gem5.opt -d /tmp/sdbp-bypass-timing \
  tests/gem5/sdbp_bypass/run.py /tmp/sdbp-traffic
build/RISCV/gem5.opt -d /tmp/sdbp-bypass-atomic \
  tests/gem5/sdbp_bypass/run.py /tmp/sdbp-traffic --atomic
build/RISCV/gem5.opt -d /tmp/sdbp-bypass-disabled \
  tests/gem5/sdbp_bypass/run.py /tmp/sdbp-traffic --disable-bypass
```

The guest checks initially clean loads and four store/readback sweeps. Its large
instruction sequence exercises read-only L1I victims with `writeback_clean=False`.
The test forces dead predictions with threshold zero and checks that L2 stays
empty, both L1s receive marked fills, and all routed writebacks reach a large L3
without memory writebacks. The control disables bypass and verifies the same
program's data while all bypass counters remain zero.

Results on October 29, 2026:

- Simulator build and both unit-test binaries pass: 26 tests total.
- Timing and atomic regressions each pass: 5,138 bypasses, 4,617 L1D fills,
  521 L1I fills, 5,106 L2 forwards and L3 receives, zero L2 occupancy and zero
  L3-to-memory writebacks. The disabled control passes with zero bypass traffic.
- `idealkmhv3.py` CoreMark smoke: one million instructions with difftest enabled
  and forced dead predictions pass, using SimpleMemory for the local check.
- `idealkmhv3.py` SPEC06 `mcf/10206` checkpoint: one million instructions with
  forced dead predictions, SimpleMemory and the baseline CI's pinned
  `normal-dedup` REF pass difftest. L1D installs 127,525 bypassed fills and L3
  receives 126,597 routed writebacks. Pending traffic may remain at the stop tick.
- Black 22.6.0, the repository modified-region style checker, and
  `git diff --check` pass.

The local checks cover single-core operation; multicore stress and the full SPEC
performance result require separate runs. No performance improvement is assumed.
