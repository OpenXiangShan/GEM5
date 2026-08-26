# Mockingjay L2 Implementation Contract

## Scope and Ownership

The target is the aligned L2 path used by `configs/example/kmhv3.py`.
`L2CacheWrapper` routes requests and owns the shared wrapper-level prefetcher,
but each `L2CacheSlice` owns a separate `inner_cache`. The replacement policy
must be attached to each `inner_cache`, so every `(CPU, slice)` has independent
sampled-cache, RDP, per-set clocks, and ETR state. No predictor state crosses
slice boundaries.

The implementation adds `MockingjayL2RP` under
`src/mem/cache/replacement_policies/`, exposes it as a SimObject, and creates a
fresh object in the `kmhv3.py` loop over `l2_wrapper.slices[j]`. This is the
replacement-policy extension point used by the existing `XSDRRIPRP`; it keeps
the wrapper pipeline and routing behavior unchanged.

## Modeling Contract

| Item | Contract |
| --- | --- |
| Performance problem | Model capacity misses caused by poor L2 eviction order and bypass opportunities. |
| Observable effects | A line can be inserted/promoted, selected as a positive/negative-ETR victim, or bypassed; normal cache timing and coherence continue through gem5's existing paths. |
| Access state machine | `hit -> sampled-history update -> set aging -> promotion`; an incoming fill first selects a victim or bypasses from the pre-update state, then its sampled-history update and set aging occur. An admitted fill finally receives its insertion ETR. |
| Resource state | Fixed-size RDP, fixed sampled-cache buckets, bounded per-set line pointer lists, per-line ETR, and per-set clocks/timestamps. |
| Distance domain | Number of accesses to the same physical L2 slice set, never global accesses, cycles, or instructions. |
| Hot-path complexity | O(1) RDP/sample lookup plus O(ways) aging on one periodic set event; victim selection O(ways). The target slice is 8-way. |
| Functional boundary | Replacement does not alter hit/miss lookup, coherence state, MSHR arbitration, or slice routing. An explicit bypass for an eligible timing fill is completed directly from the response packet, without creating a temporary cache block or retaining the line. |

## Packet-Aware Bypass Extension

The existing replacement interface passes `PacketPtr` to `touch` and `reset`,
but not to `getVictim`. The paper's bypass decision needs the incoming PC,
hit/miss, and prefetch state before a resident victim is evicted.

The implementation therefore adds a backward-compatible packet-aware victim
path:

```c++
getVictim(candidates, pkt)
```

`BaseTags::findVictim` accepts an optional `policy_bypassed` result. Its base
implementation clears that result and delegates to the existing packet-less
overload, so tag stores and policies without direct-bypass support retain their
behavior. `BaseSetAssoc` calls `getVictim(candidates, pkt)` only when the
caller supplies that result pointer; otherwise it uses the historical
packet-less victim selection. A null victim is an explicit bypass only when
`policy_bypassed` is true. Other allocation failures keep the normal
temporary-block behavior.

For an explicit bypass, `BaseCache::handleFill` returns `nullptr` before it
creates `tempBlock`, installs tag/data/coherence state, or updates fill
metadata. `recvTimingResp` then services the target from the lower-level
response packet through the existing `Cache::serviceMSHRTargets` direct
response path. That path copies response data and responder flags to the
target. Because no temporary block exists, the later temporary-block cleanup
does not call `evictBlock`; in particular, the bypass cannot generate a
temporary-fill `WritebackClean`.

The response path also suppresses `notifyCachelineRefill` and the `Fill` probe
for an explicit bypass. This prevents cacheline-refill and prefetch feedback
from observing a line that was never resident.

### Direct-Response Bypass Eligibility

Direct response is deliberately narrower than the policy's prediction rule.
It is enabled only for an allocating, non-error L2 timing fill when all of the
following are true:

* the cache is `cacheLevel == 2` and the MSHR is not a forward;
* the MSHR has exactly one target, from the CPU side, and no prefetch target;
* the target command is exactly `ReadSharedReq`, with no writable requirement
  and no whole-line write;
* the lower response is clean (`cacheResponding == false`) and the MSHR has no
  pending downgrade or invalidation;
* the target is neither a hardware/software prefetch, LL/SC, locked RMW,
  read-modify-write, atomic/swap, cache-maintenance, nor uncacheable request.

This excludes multi-target MSHRs, coherence-owning operations, and requests
whose completion relies on a resident cache block. In particular,
`ReadCleanReq`, dirty lower responses, and MSHRs that observed a snoop are
intentionally ineligible: normal allocation changes their response semantics
before an upper cache sees them. A dirty lower response must be converted into
a Shared response after an allocated fill, and a concurrent read snoop sets
`postDowngrade` even when it does not add an MSHR target. Atomic callers and
all ineligible timing fills use the legacy victim-selection path, so they
cannot turn a policy `nullptr` into a direct bypass.

VIPT tags retain their virtual-address indexing while forwarding the original
packet to the new overload. This keeps the interface extension neutral outside
Mockingjay.

## Data Structures and Defaults

All sizes are parameters. The `kmhv3.py` defaults are derived from the actual
per-slice cache geometry rather than hard-coded constants:

| Parameter | Initial value for 512 KB, 8-way slice | Meaning |
| --- | ---: | --- |
| `num_sets` | `inner_cache.size / (64 * inner_cache.assoc)` = 1024 | Per-slice set count |
| `num_ways` | `inner_cache.assoc` = 8 | Per-slice associativity |
| `block_bits` | 6 | 64 B cache line |
| `slice_bits` | 2 | Interleaved slice selector removed before sampled-cache extraction |
| `history_multiplier` | 8 | Paper's history length in units of ways |
| `aging_granularity` | 8 | One ETR decrement per eight accesses to a set |
| `sampled_sets` | 8 | Scaled from one sampled set per 64 KiB of L2 capacity |
| `sampled_cache_sets_per_set` | 16 | Low block-tag buckets per sampled physical set |
| `sampled_cache_ways` | 5 | Paper default |
| `sampled_tag_bits` | 12 | `31 - log2(512 KiB)` truncated sampled tag width |
| `rdp_entries` | 512 | `2^(log2(512 KiB) - 10)`, a 9-bit direct-mapped predictor |
| `temporal_difference_threshold` | 16 | Public reference's integer interpretation of `diff / 16` |
| `scan_threshold_margin` | 22 | Public reference's `MAX_RD = INF_RD - 22` rule |
| `prefetch_penalty_percent` | 200 | Single-core paper default for `*-P` intervals |

Derived values are `INF_RD = num_ways * history_multiplier - 1`,
`MAX_RD = INF_RD - scan_threshold_margin`, and
`INF_ETR = (num_ways * history_multiplier / aging_granularity) - 1`.
For the initial 8-way slice they are 63, 41, and 7 respectively. Parameters
are validated for nonzero values and power-of-two geometry where indexing
depends on it.

`MockingjayReplData` holds `valid`, `set_id`, `way_id`, and signed `etr`.
The policy owns:

* `entries_by_set`: bounded pointers to the replacement data of every way;
* a per-set aging clock;
* sampled-set timestamps and a fixed 5-way sampled-cache bucket array;
* a direct-mapped RDP vector with valid bit and predicted reuse distance,
  indexed by the low `log2(rdp_entries)` bits of the PC/state CRC hash.

No dynamically growing lookup map is used in the access path.

The paper specifies the least-significant bits of the CRC hash for the RDP
signature. The preserved public ChampSim source instead extracts the high bits.
For the RV64 PC range used here, that form collapses ordinary PCs into entry
zero after its three-step CRC transform, so this port follows the paper and
uses the low hash bits.

## Algorithm Details

1. `touch(data, pkt)` handles an L2 hit. It records a hit signature,
   processes sampled history when this is a sampled set, performs periodic
   set aging, and promotes the line by replacing its ETR with the RDP result.
2. For a demand fill, `getVictim(candidates, pkt)` first predicts and selects
   from the resident pre-update state. A predicted bypass then records the
   miss in sampled history and ages the set, but has no line ETR to assign.
   An admitted fill reaches `reset(data, pkt)`, which records the miss,
   performs the same sampling/aging sequence, and assigns the predicted ETR.
   Thus every bypassed or admitted demand fill trains exactly once after its
   selection decision. Writeback fills receive the low-priority negative scan
   ETR but are never bypassed.
3. Sampled history trains the prior PC signature on reuse and trains a scan
   when an aged-out or LRU sampled entry is displaced. `*-P` intervals inflate
   the sampled distance before training. The reference implementation's
   integer temporal-difference behavior is used: a non-scan prediction moves
   by one only when the distance differs by at least 16; a scan observation
   detrains by one toward `INF_RD`.
4. Every eight accesses to a set, each other valid non-scan way's ETR is
   decremented and clamped at `-INF_ETR`. Scan ETRs are not aged.
5. Victim selection first returns an invalid way. Otherwise it finds maximum
   `abs(ETR)`, breaking ties in favor of negative ETR. For packet-aware
   incoming fills, it bypasses if the prediction is a scan or its ETR is
   strictly larger than the selected victim's absolute ETR. Equal priority
   inserts so the deterministic resident tie-break remains observable.

Requests without a PC use a reserved no-PC signature. They remain cacheable
and are counted, but their predictor correlation is intentionally isolated
from ordinary load PCs. Packet-less `touch/reset` paths only maintain valid
replacement state and do not train the predictor.

## Observability

The policy exports counters for sampled hits/misses, reuse and scan training,
RDP lookup hits/misses, no-PC accesses, promotions, periodic aging events,
insertions, bypasses, and positive/negative ETR victims. These distinguish
learning activity from eviction outcomes and support a finite-size stress test.

## Validation Plan

1. Compile generated SimObject parameters and the optimized RISC-V binary.
2. Add focused gtests for invalid priority, sampled reuse, scan detraining,
   per-set isolation, signed ETR tie-break, no-PC behavior, and bypass.
   Add cache-level timing tests for the clean direct-bypass case, dirty and
   `ReadCleanReq` exclusions, and a concurrent read snoop that sets
   `postDowngrade` and must force allocation.
3. Run a short checkpoint smoke test with enlarged structures, confirm policy
   construction and nonzero/consistent stats, then restore paper-scaled
   defaults.
4. Run the requested omnetpp/6881 GCC15 checkpoint with `kmhv3.py`, inspect
   `config.ini`, exit status, and Mockingjay counters.
5. After local validation, publish the branch and present the fully resolved
   GCC15 SPEC06 1.0c CI contract for explicit dispatch approval. Compare the
   completed archive and `score.txt` with run 32391965338 at the same base SHA.

## Accuracy Boundaries

This is a behavioral performance model, not an RTL implementation. It keeps
the paper's predictor, per-set time domain, ETR ordering, bypass criterion, and
prefetch interval penalty. It intentionally reuses gem5's event-driven cache
pipeline rather than modeling a separate off-critical-path hardware engine.
The paper's source does not fully specify same-sign ETR ties, fractional TD
rounding, or lookup/increment ordering; this implementation fixes those choices
as deterministic rules above and tests them. The 8-bit per-set timestamps keep
the paper's bounded-history representation: they distinguish one wrap but can
alias an entry left untouched for a full 256 sampled-set accesses. Entries older
than the configured history are detrained on their next sampled-cache bucket
access, which makes that case a bounded, inherited approximation rather than a
cycle-accurate timestamp model.
