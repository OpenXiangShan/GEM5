# SDBP L2 refill bypass

The Kunminghu SDBP configuration enables bypass by default. Use
`--no-sdbp-enable-bypass` for the replacement-only control. Other replacement
policies retain their existing behavior. Bypass requires an L3 cache.

A predicted-dead data refill uses the L2 temporary block instead of installing
L2 tags, even if an invalid way is available. Requests without a PC, actual
prefetches, upgrades, writebacks and cache maintenance do not bypass. A bypassed
miss still updates the independent sampler, so the predictor can learn reuse.

The response marks the receiving L1 line with `sdbpBypassL2`, independently
of its coherence dirty bit. A clean fill remains clean in L1. On replacement,
a marked clean victim produces a data-bearing `WritebackClean`; a genuinely
modified victim retains the normal `WritebackDirty` command. The marker routes
both commands through the L2 write buffer without allocating L2 tags.

If a later request has installed an L2 copy, the returning victim invalidates
that copy before forwarding. If that L2 copy owns real dirty data, its dirty
responsibility is transferred along with the writeback before invalidation.
At L3, `WritebackClean` allocates or updates data without setting the dirty bit;
`WritebackDirty` preserves real modifications. The L3 `writeback_clean` parameter
controls the packets L3 generates on its own evictions, not whether it accepts
incoming `WritebackClean` packets. Existing clean-writeback/MSHR and peer-copy
filtering remains in effect to preserve coherence ordering.

In this source tree `CleanEvict` has no payload; `WritebackClean` has data.
The bypass marker forces L1 to generate a data-bearing writeback even when
its normal `writeback_clean` option is disabled. Snoop packet copies do not
inherit the route. Block invalidation clears the marker and block moves preserve
it. Timing and atomic accesses use the same refill and eviction rules. Existing
write-buffer backpressure and arbitration apply.

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
empty, both L1s receive marked fills, and routed writebacks reach L3. The control
disables bypass and verifies the same program's data with zero bypass traffic.

Additional tests force L3 replacements. A read-only guest must generate clean
writebacks to L3 without creating dirty writebacks or memory writes. A mixed
read/write guest with a small L3 must preserve modified data across L3 evictions
and produce genuine dirty writebacks:

```sh
riscv64-linux-gnu-gcc -DSDBP_READ_ONLY -nostdlib -static \
  -march=rv64im -mabi=lp64 -Wl,--build-id=none \
  -o /tmp/sdbp-readonly tests/gem5/sdbp_bypass/traffic.S
build/RISCV/gem5.opt -d /tmp/sdbp-clean-small \
  tests/gem5/sdbp_bypass/run.py /tmp/sdbp-readonly --clean-only --small-l3
build/RISCV/gem5.opt -d /tmp/sdbp-mixed-small \
  tests/gem5/sdbp_bypass/run.py /tmp/sdbp-traffic --small-l3
```

Repeat both tests with `--atomic`. The default 128 KiB L3 also checks that no
writebacks escape to memory while the working set fits. `--small-l3` uses 8 KiB.
Add `--exclusive-l3` to the read-only test to make L3 drop clean demand refills;
the test then checks that returning `WritebackClean` packets allocate previously
absent L3 lines. This verifies that acceptance is independent of the L3
`writeback_clean=False` emission setting.

Validation on October 29, 2026:

- `gem5.opt` builds; all 26 SDBP core/policy unit tests pass.
- All 11 targeted SE regressions pass: timing/atomic, clean-only/mixed traffic,
  large/small L3, a bypass-disabled control, and clean-only exclusive L3.
- Clean-only cases deliver all 611 routed victims to L3 as `WritebackClean`,
  with zero dirty writebacks, zero memory writebacks and zero L2 occupancy.
- Exclusive L3 retains returning clean blocks despite dropping demand refills,
  with positive resident tag occupancy and `writeback_clean=False`.
- Mixed traffic with 8 KiB L3 preserves guest data across 2,048 real dirty
  L3-to-memory writebacks; 128 KiB L3 generates no memory writebacks.
- `idealkmhv3.py` SPEC06 `mcf/10206` runs one million instructions with forced
  dead predictions and the CI-pinned `normal-dedup` REF, without difftest errors.
  SimpleMemory is used for this local smoke check.
- The repository modified-region style checker, Black 22.6.0 and
  `git diff --check` pass.

The local checks cover single-core operation; multicore stress and the full SPEC
performance result require separate runs. No performance improvement is assumed.
