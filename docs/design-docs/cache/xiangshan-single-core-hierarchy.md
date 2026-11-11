# XiangShan single-core cache hierarchy policy

`xiangshan_single_core` is a cache hierarchy behavior policy, independent of
LRU, XS-DRRIP, SDBP, and SDBP's bypass prediction. It aligns the model with the
XiangShan single-core behavior specified for these experiments:

- Ordinary data reads receive writable permission when the responding cache
  has writable permission and no actual sharer or pending downgrade prevents it.
- Writable permission and dirty data are independent. Receiving a clean,
  writable line does not require a data writeback when L1 evicts it.
- L1 victims return data only when dirty. Dirty L1 victims marked as having
  bypassed L2 are forwarded through L2's write buffer to L3 without L2 allocation.

This is functional and resource modeling alignment with the stated single-core
behavior. It is not a claim of complete TileLink or cycle-level RTL equivalence.
At the L2-to-L1D boundary, dirty responsibility stays at L2 while L1D
records its own writes. L2's dirty bit does not guarantee that its data is
current when L1D has writable permission. Snoops and retained-dirty victim
writebacks must therefore collect any newer upper data first. A downstream
snoop racing a pending victim is deferred in that victim's reserved write
buffer transaction; it cannot respond with the old payload or free the slot
before the upper response arrives. L3-to-L2
ownership transfer retains the classic protocol. This does not implement
RTL ProbeQueue/mainpipe arbitration, the RTL's non-coherent L1I, or its SDBP
predictor algorithm.

## Selection and scope

Both `configs/example/kmhv3.py` and `configs/example/idealkmhv3.py` accept:

```sh
--cache-hierarchy-policy=xiangshan_single_core
```

The default is `legacy`. Configuration applies the selected policy to the
single core's L1s, walker caches, L2 slices (or classic L2), and L3, after the
platform and replacement settings have been applied. The selector does not
change cache capacity, associativity, latency, clusivity, prefetch settings, or
replacement parameters. One CPU, no SMT, classic caches, and an L1/L2/L3
hierarchy are required. The C++ policy also rejects a multi-core System.

Examples, with all other workload/cache settings held equal:

```sh
# SDBP replacement, XiangShan single-core hierarchy behavior, bypass off.
--cache-hierarchy-policy=xiangshan_single_core \
--l2-replacement-policy=sdbp --sdbp-sampler-num=128 --sdbp-sampler-assoc=6

# Same hierarchy behavior, bypass on.
--cache-hierarchy-policy=xiangshan_single_core \
--l2-replacement-policy=sdbp --sdbp-sampler-num=128 --sdbp-sampler-assoc=6 \
--sdbp-enable-bypass

# The same grants and L1 eviction behavior with LRU.
--cache-hierarchy-policy=xiangshan_single_core --l2-replacement-policy=lru
```

The former `--[no-]sdbp-bypass-writable` and `sdbp_bypass_writable` cache parameter
have been removed. They forced ordinary reads to `ReadExReq` only in the bypass
experiment, changing request type and permission behavior together. Use the
same hierarchy policy on both sides of a bypass comparison instead.

## Ordinary reads and MSHR ordering

The normal downstream command remains `ReadSharedReq`; no invalidating
`ReadExReq` is inserted for an ordinary read. Before issuing the request, each
requesting cache sets a transient `writableFillExpected` promise. After a
successful send, lower caches prepare their existing MSHR ownership state
unless a real sharer has already been reported. Ordinary L1 read MSHRs do
not promise dirty ownership merely because they may receive writable data.
Explicit write/upgrade targets still prepare ownership through needsWritable;
merged writes can also be promoted once the writable response arrives.

The promise is not a dirty bit or an unconditional grant. A responder must
still have `WritableBit`, no `hasSharers`, no pending downgrade, and an eligible
ordinary data read. L2 keeps its dirty obligation when granting data or an
upgrade to L1D, including after its own miss (`deferred_response`). L3 can
still transfer dirty ownership to L2. Legacy gem5 conservatively returns
Shared in the deferred dirty case where the upstream MSHR did not prepare
for an unsolicited ownership transfer.

Packet copies, including snoops, start without this promise. Each cache arms
its own downstream request; it must not accidentally authorize an unrelated
snoop. Actual shared copies and deferred invalidations retain the existing
coherence handling. Instruction fetches, read-only caches, prefetches,
`ReadCleanReq`, uncacheable traffic and explicit ownership requests retain their
existing request semantics. The selector is intentionally limited to a single
core and is not a new general multi-core coherence protocol.

A data read can leave L1D writable and clean even when L2 is dirty relative
to L3. A later L1 write marks L1 dirty without clearing the L2 obligation;
both levels may then be dirty and only L1 has the latest data. The model
forwards an upper response instead of also responding with stale L2 data.
Functional snoops from below also search upper caches before trusting L2's
dirty bit. An upper WriteClean supplies the latest maintenance data, avoiding
a second stale WriteClean from L2. Multiple writable states along one path
do not permit independently writable sibling copies.

## Victims and data-array resources

The policy makes every clean L1 eviction use `CleanEvict`, even if
`writeback_clean=True` was requested on L1. This preserves a control notification
for the existing snoop filter but transfers no cache-line data. Dirty L1 victims
use `WritebackDirty` and copy the data. Writable permission alone is not tested
to decide whether an eviction has data.

The existing L1D refill/mainpipe resource model obtains `needDataRead` from the
evicted victim's dirty state. It reserves the data-read banks and writeback
port for dirty victims, and omits those resources for clean victims. Incoming
refill data still needs its own tag/data-write resources. This change does not
introduce a new data-array timing model or change unrelated LSQ behavior.

L2/L3 retain their configured `writeback_clean` and clusivity semantics. In
particular, an L2 temporary bypass fill retains its normal eviction handling;
suppressing its clean data unconditionally would alter LLC residency when L3
is mostly exclusive.

## Retained dirty victims

With `--l2-evict-invalidate-l1`, the existing timed victim probe collects
upper dirty data before releasing the L2 write buffer. If this latency option
is disabled, resident dirty L2 victims still use that recovery path for
correctness: retained responsibility cannot authorize writing stale data.
Clean victims retain their existing behavior. Atomic mode performs the
corresponding synchronous probe before sending a retained-dirty victim.

Temporary bypass fills never occupied a real L2 way and do not invalidate
the response being sent to L1. If such a fill retained a dirty obligation,
its normal temporary-block writeback carries that obligation to L3. L1's
routing marker does not make the line dirty; a later actual L1 write uses
the existing marked writeback route to L3.

## L2 bypass routing and sampler boundaries

SDBP determines whether a full-set allocation is bypassed. The decision is
carried as generic `bypassL2` transport/block metadata:

1. L2 observes the completed fill in the sampler exactly once, even when using
   its temporary block rather than a real cache way.
2. Under this hierarchy policy, the L2 response marks L1's installed block.
3. A clean marked L1 block sends only `CleanEvict`; a dirty marked block sends
   `WritebackDirty` carrying `bypassL2`.
4. L2 routes that writeback through its normal write buffer, preserves ordering
   with same-address misses, and does not allocate an L2 way. A later installed
   same-address L2 copy is invalidated, preserving any actual dirty obligation.
5. L3 consumes the routing marker and handles the packet normally.

Block moves transfer the marker and invalidation clears it. It is independent
of coherence state and never marks data dirty by itself. The policy keeps
existing port delays, write-buffer limits and backpressure. It does not add an
ideal path around the modeled buses.

In `legacy`, the new default grants, dirty-only L1 override, and L2-bypass
victim routing are disabled. SDBP's independent sampler observation and
prefetch/demand training fixes remain algorithm correctness fixes in either
mode. L1-copy protection and sampler-youngest protection remain separate SDBP
replacement parameters, not hierarchy policy settings.

## Counters and implementation

Cache counters are independent of SDBP:

| Counter | Meaning |
|---|---|
| `writableReadExpectations` | Successfully issued ordinary reads whose MSHRs prepare for a writable response; timing mode |
| `defaultWritableGrants` | Ordinary read responses granted writable under this policy |
| `deferredWritableGrants` | Deferred dirty read responses allowed to transfer writable ownership |
| `retainedDirtyGrants` | L2 data/upgrade grants that retain an existing dirty obligation |
| `retainedDirtyUpperResponses` | L2 snoops using an upper response instead of its retained copy |
| `retainedDirtySnoopDeferrals` | Downstream snoops waiting for complete victim data |
| `retainedDirtyVictimProbes` | Correctness recovery of dirty L2 victims without the timed-victim option |
| `bypassL2Fills` | L1 fills marked for later L2-bypass eviction |
| `bypassL2Writebacks::{clean,dirty}` | Marked writebacks generated by L1, forwarded by L2, or consumed at L3 |
| `bypassL2CleanEvictions` | Marked clean L1 evictions without data |

The old `sdbpWritableReadMisses` has no equivalent: this policy does not force
ordinary reads into ownership request commands. Former `sdbpBypass*` cache
counters are now `bypassL2*`. Predictor training counters, including
`samplerBypassAccesses`, retain their original names and meaning.

- `src/mem/cache/hierarchy_policy.hh`: named policy decisions, O(1) per event.
- `configs/common/CacheHierarchy.py`: independent CLI and hierarchy selection.
- `src/mem/cache/cache.cc`: permission grants, ordinary miss packets, L1 victims.
- `src/mem/cache/base.cc`: MSHR preparation, fills, writeback routing and counters.
- `src/mem/packet.hh`, `src/mem/cache/cache_blk.hh`: transient transport/state.

No predictor is queried on the hierarchy policy path. Existing bounded SDBP
sampler/replacement work is unchanged.

## Validation

The policy tests and three-level integration workload cover ordinary read
commands, promise isolation from packet copies/snoops, clean permission without
dirty state, legacy behavior, bypass marker moves/invalidation, timing/atomic,
bypass on/off, SDBP/LRU, clean-only execution, normal predictor training and
L1 `writeback_clean` overrides. Mixed read/write workloads verify data after
reuse beyond LLC capacity and exercise deferred dirty grants.

```sh
scons build/RISCV/gem5.opt \
  build/RISCV/mem/cache/hierarchy_policy.test.opt \
  build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt \
  build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt --gold-linker -j64

build/RISCV/mem/cache/hierarchy_policy.test.opt
build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt
build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt

# The integration script defaults to xiangshan_single_core.
build/RISCV/gem5.opt --outdir=m5out/xs-cache-policy \
  tests/gem5/cache_eviction/hierarchy_bypass.py /tmp/sdbp-bypass-workload --bypass
build/RISCV/gem5.opt --outdir=m5out/xs-cache-lru \
  tests/gem5/cache_eviction/hierarchy_bypass.py /tmp/sdbp-bypass-workload --replacement=lru
```

To exercise dirty ownership passing specifically after an L2 miss, compile
`bypass_workload.c` with `-DLINES=256` using the same freestanding build flags.
The combined clean/dirty footprint then exceeds L2 but fits L3. Run it with
`hierarchy_bypass.py ... --deferred`, both with and without `--bypass`; the test
requires nonzero `system.l2.deferredWritableGrants` and correct data reuse.

Performance validation must use the same hierarchy policy for bypass on/off,
a pinned binary/REF/checkpoint, matching generated configurations, and the
measurement stats block after warmup. Changes in bus arbitration or the L3
slice busy model are outside this strategy and require separate evidence.

The source worktree records a prior standalone-policy validation on
2026-11-09, before integration with the baserun three-source predictor and L2
victim latency policy below. That record passed 7 hierarchy-policy tests,
13 SDBP core tests,
16 SDBP interface tests, and 23 three-level integration cases. Seven checkpoint
runs (three libquantum slices with policy/bypass on and off, plus one legacy
control) completed 20M warmup + 20M measurement with difftest. Both policy groups
retained ordinary shared-read requests and had zero L1-to-L2/L2-to-L3 UpgradeReq.
The legacy control reproduced 4,769,717 cycles for libquantum/11901 exactly.
With this same policy on both sides, bypass IPC changes for 11901/69175/86704
were -2.904% / +3.429% / +5.329%; this is a three-slice check, not a full SPEC score.


## Combined experiment

This branch retains the selected baserun three-source predictor, which excludes
instruction fetches, and uses the hierarchy policy with every replacement
variant. Only demand refills bypass a full set; invalid ways retain priority.
Bypassed fills are observed exactly once in BaseSetAssoc::findVictim, while
normal allocated fills are observed in insertBlock. The root worktree's old
single-source predictor and unrelated CPU diagnostics were not imported.

The current five-run setup and integration validation are recorded in
`l2-eviction-l1-invalidation.md`. Two failures found in this integration were
fixed: atomic clean exclusive hits now consume their deferred tag-invalidation
flag, and Queued::createPrefetchRequest preserves an absent instruction PC
instead of asserting when a forwarded prefetch has only a trigger PC.

## Dirty-responsibility regression

`tests/gem5/cache_eviction/dirty_responsibility.S` uses five m5_exit phases
and no implicit stack stores. It first writes a line back to L2, reads and
cleanly evicts it from L1, modifies it in L1 while L2 retains the older dirty
copy, and executes enough code to force L2 replacement without unrelated
L1D accesses. The guest verifies the latest value. The final read/eviction
also exercises dirty data returning from the mostly-exclusive LLC. The
configuration checks per-phase data-writeback counts and retained-grant/
dirty-response counters, with timing/atomic and legacy controls.

```sh
riscv64-linux-gnu-gcc -nostdlib -static -march=rv64g -mabi=lp64d \
  -Wl,-e,_start tests/gem5/cache_eviction/dirty_responsibility.S \
  -o /tmp/dirty-responsibility
build/RISCV/gem5.opt --outdir=m5out/dirty-responsibility \
  tests/gem5/cache_eviction/dirty_responsibility.py \
  /tmp/dirty-responsibility --invalidate
```

`l2_invalidate.py --peer --cache-hierarchy-policy=xiangshan_single_core`
adds a coherent traffic injector below L2, exercising downward snoops and
false-sharing data checks. This is a protocol stress test, not a supported
multi-core XiangShan configuration. `--functional` adds functional accesses;
`--write-buffers=1` and `--sliced-l2` stress buffering and same-address ordering.

The complete retained-dirty suite builds its own RISC-V guests and runs phase,
three-level timing/atomic, bypass, and MemTest cases:

```sh
python3 tests/gem5/cache_eviction/run_dirty_regression.py \
  --binary=build/RISCV/gem5.opt --output=m5out/dirty-regression
```

The lower-snoop case also checks the retained victim path with CacheRepl
tracing enabled. The phase test compares legacy inherited dirty writebacks
with zero L1 data writebacks for read-only reuse under this policy. These
checks verify dirty responsibility and data integrity; they do not establish
cycle-level RTL equivalence or replace full performance evaluation.
