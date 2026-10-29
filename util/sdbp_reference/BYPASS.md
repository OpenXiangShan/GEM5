# SDBP L2 refill bypass

The Kunminghu SDBP configuration enables bypass by default. Use
`--no-sdbp-enable-bypass` for the replacement-only control. Other replacement
policies retain their existing behavior. Bypass requires an L3 cache.

A predicted-dead data refill uses the L2 temporary block instead of installing
L2 tags, even if an invalid way is available. Requests without a PC, actual
prefetches, upgrades, writebacks and cache maintenance do not bypass. A bypassed
miss still updates the independent sampler, so the predictor can learn reuse.

When bypass is enabled, `configure_sdbp` also enables
`cpu.dcache.sdbp_bypass_writable`. L1D ordinary cacheable demand read misses
request ownership with `ReadExReq` before the L2 refill-time prediction.
The L1 MSHR records that outgoing ownership request even though the CPU target
is a plain read, so it can order snoops while the response is in flight.
This applies to all such L1D misses, including those ultimately predicted live;
bypass-disabled and LRU configurations keep their original read requests.
Read-only L1I, prefetches, uncacheable accesses and special read commands keep
their existing request protocol.

Data bypass is restricted to exclusive read responses without sharers. Shared
responses use ordinary L2 allocation. Clean instruction responses can still
bypass, including shared responses, because L1I never needs write permission.
For a bypassed exclusive response, the standard `ReadExReq` handling transfers
both write permission and any genuine dirty responsibility from the L2 temporary
block to L1. The temporary block then has no dirty data to write back. L1 fills
clean exclusive data as E and genuinely dirty data as M; later snoops can revoke
permissions normally. Deferred snoops propagate the upstream MSHR responder
identity so only the new owner answers a previously promised snoop response.

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

## Ownership handoff regression

The mixed guest now stores each verified value immediately after reading it,
including values returned from dirty L3 lines. With threshold zero, the test
checks that L2 remains empty, L2 temporary blocks emit no dirty writebacks, and
no permission upgrades reach L3. Clean-only and small-L3 cases retain their
checks for data-bearing clean returns and real dirty memory writebacks.

A separate two-core test shares a physical page between two SE processes. It
passes a sequence number and a turn flag across private L1/L2 hierarchies for
256 handoffs in each direction, with long response latencies to exercise snoops
of in-flight exclusive fills. Both guests verify the transferred data. Run it
with and without bypass:

```sh
riscv64-linux-gnu-gcc -DROLE=0 -nostdlib -static \
  -march=rv64im -mabi=lp64 -Wl,--build-id=none \
  -o /tmp/sdbp-producer tests/gem5/sdbp_bypass/shared.S
riscv64-linux-gnu-gcc -DROLE=1 -nostdlib -static \
  -march=rv64im -mabi=lp64 -Wl,--build-id=none \
  -o /tmp/sdbp-consumer tests/gem5/sdbp_bypass/shared.S
build/RISCV/gem5.opt -d /tmp/sdbp-shared \
  tests/gem5/sdbp_bypass/coherence.py /tmp/sdbp-producer /tmp/sdbp-consumer
build/RISCV/gem5.opt -d /tmp/sdbp-shared-control \
  tests/gem5/sdbp_bypass/coherence.py /tmp/sdbp-producer /tmp/sdbp-consumer \
  --disable-bypass
```

This is a targeted coherence check, not exhaustive multicore verification.
Performance must be evaluated with the requested full CI configuration; giving
read misses exclusive ownership can increase invalidations under sharing.
Add `--shared-reader` to let the consumer retain ordinary shared reads while
the producer requests ownership. This also exercises deferred downgrading
snoops and verifies that shared data falls back to ordinary L2 allocation.

Validation on October 30, 2026:

- Optimized simulator build and all 27 SDBP core/policy unit tests pass.
- All 11 SE regression cases pass, now including immediate read-then-write
  checks on previously dirty lines. Forced dead fills leave zero L2 occupancy,
  zero L2-generated writebacks and zero L3 permission upgrades.
- Clean-only cases still deliver 611 `WritebackClean` packets and generate no
  dirty writebacks or memory writes. The modified mixed small-L3 guest preserves
  its data across 3,968 genuine dirty memory writebacks.
- The two-core shared-page test passes with bypass enabled, disabled and with
  an ordinary shared consumer. The enabled run's MSHR trace exercises 1,537
  pending-ownership snoops with deferred invalidations. The mixed-reader trace
  includes 767 post-downgrade and 514 post-invalidation MSHR states, exercising
  both kinds of ownership conflict and migration of the promised responder.
- `idealkmhv3.py` libquantum/46670 passes difftest for 3 million instructions
  (1 million warmup, 2 million measured) using SimpleMemory and the CI-pinned
  `normal-dedup` reference. Its measured 25,484 L1D writes all hit and no
  `UpgradeReq` reaches L3. This short check does not predict full CI performance.

## Sampler recency protection with bypass

The Kunminghu SDBP CLI now defaults to 128 sampled sets per L2 slice and a
six-way independent sampler. The real L2 remains eight-way. With four slices,
this uses 3,072 sampler entries instead of the previous 4,096. Predictor table
sizes, threshold, L1-copy protection and bypass ownership handling are unchanged.

`--sdbp-sampler-protect-youngest-two` is enabled by default in the Kunminghu
configuration. On a full sampler set, exclude its MRU and second-MRU entries
from the dead-first victim scan. Compute recency across both live and dead
entries, and keep the original candidate order for remaining dead entries.
Invalid entries still take priority; the fallback is global LRU, including
protected positions. Skipping an entry does not clear its dead prediction.
Prefetch hits retain their existing behavior and do not update recency.

The algorithm and its targeted tests are adapted from commit `39a2db6f92`.
The generic SDBPCore and SDBPRP parameter default remains false for compatibility;
the Kunminghu configuration enables it explicitly. To reproduce the prior
platform sampler, use both `--sdbp-sampler-assoc=8` and
`--no-sdbp-sampler-protect-youngest-two`. Bypass stays independently configurable.

The six-way protected sampler also replaces the four-way test sampler in the
standalone SE bypass and coherence regressions. Core tests cover protected
positions, original dead-candidate order, invalid priority, unchanged dead bits,
prefetch-hit recency and global LRU fallback with one or two ways. A policy test
checks parameter propagation. The full-system smoke check verifies that every
L2 slice receives 128 sampled sets, six ways and enabled recency protection,
while L1D continues to request ownership for bypass-compatible fills.

This changes sampler residency and predictor training, so its effect on the
bypass version must be measured independently of the source experiment, which
had bypass disabled. No extra hardware lookup latency is modeled by the guard.

Combined-version validation:

- Optimized build and all 34 core/policy unit tests pass.
- All 14 SE cases pass with protected six-way samplers: 11 timing/atomic bypass
  cases plus the enabled, disabled and shared-reader two-core regressions.
- libquantum/46670 passes the same 3M-instruction difftest smoke run with 1M
  warmup and approximately 2M measured instructions. No upgrade reaches L3.
- Its actual config.json confirms 128 sampled sets, six ways, recency protection
  and bypass enabled in all four L2 slices, plus L1D ownership requests.
