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
In particular, inherited dirty ownership still follows gem5's existing rule:
when dirty responsibility is transferred upward, the receiver becomes dirty,
even if it has not executed a local store. A full RTL comparison of that
responsibility placement remains separate work.

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
successful send, that promise prepares the MSHR's existing pending ownership
state, unless a real sharer has already been reported.

The promise is not a dirty bit or an unconditional grant. A responder must
still have `WritableBit`, no `hasSharers`, no pending downgrade, and an eligible
ordinary data read. It may then transfer dirty ownership upward even when
responding after its own miss (`deferred_response`). Legacy gem5 conservatively
returns Shared in that deferred dirty case because the upstream MSHR did not
prepare for an unsolicited ownership transfer.

Packet copies, including snoops, start without this promise. Each cache arms
its own downstream request; it must not accidentally authorize an unrelated
snoop. Actual shared copies and deferred invalidations retain the existing
coherence handling. Instruction fetches, read-only caches, prefetches,
`ReadCleanReq`, uncacheable traffic and explicit ownership requests retain their
existing request semantics. The selector is intentionally limited to a single
core and is not a new general multi-core coherence protocol.

Clean grants remain Exclusive (writable, clean). Dirty grants transfer the
existing dirty responsibility as Modified. Having multiple writable states
along the same inclusive hierarchy path follows the existing gem5 convention;
it does not permit independent writable sibling copies.

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
