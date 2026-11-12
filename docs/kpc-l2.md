# KPC-R insertion at L2

## References and source search

CRP (*Cache Replacement Policies*, Jain and Lin, 2019), section 5.4.1,
points to Jinchun Kim et al., *Kill the Program Counter: Reconstructing
Program Behavior in the Processor Cache Hierarchy*, ASPLOS 2017,
pp. 737–749, DOI [10.1145/3037697.3037701](https://doi.org/10.1145/3037697.3037701).

The author-hosted [paper](https://people.engr.tamu.edu/djimenez/pdfs/p737-kim.pdf)
is downloaded locally as `docs/papers/kpc-asplos2017.pdf` (not committed).
SHA256: `aaa514c162e1a01bda70b8749174ba8992980a500d6db6281587d26846fd3a24`.

A GitHub source search on 2026-11-13 for `"KPC-R"`, `"KPC" "PGH"`,
`"global_hysteresis" cache`, and repositories matching `KPC cache prefetch`
did not find the original implementation. The upstream
[ChampSim tree](https://github.com/ChampSim/ChampSim) includes `spp_dev`,
but this is not evidence of an original KPC-R implementation. The previously
reported `RISE-COCO/ChampSim` repository returns 404. This implementation is
therefore derived from the paper, not claimed to be imported author code.

## Mechanism and deliberate adaptations

KPC-R section 3.2 and Table 1 specify two global 3-bit saturating hysteresis
counters, DGH and PGH, trained by an independent true-LRU sampler. On the
first sampler reuse, decrement the counter belonging to the entry's original
allocation type. Further hits only refresh LRU order. On a sampler miss,
an unused valid LRU victim increments its allocation-type counter. Invalid
and already reused victims do not train. Neither actual-cache eviction nor
invalidation removes a sampler entry.

Demand and prefetch fills use RRPV 3 when their respective counter is
saturated, otherwise RRPV 2. Demand hits promote to RRPV 0. Victim selection
prefers invalid entries; otherwise age every way until an RRPV-3 candidate
exists, breaking ties by way order. Replacement and sampling are bounded by
associativity; there is no cache-wide scan on accesses. Sampler lookup/training has no separately
modeled latency or bandwidth limit; this is a replacement-policy abstraction.

The original operates at LLC. Here it operates at private L2. Every slice of
one core shares a single `KPCReplState`, including both counters and a sampler
of 64 sets across the complete L2. A fixed integer hash selects exactly 64
sets without consuming gem5's random stream. The indexing policy supplies
the actual local set, including slice-bit removal; `(local_set * slices +
slice)` identifies the global set. Each shadow set has L2 associativity.
Full physical line addresses and the secure bit avoid partial-tag aliases.
Section 3.2 says 64 *sets*, while Table 3 budgets 64 *entries*; this model
follows the prose and reports the resulting larger sampler explicitly.
For 2 MiB, 8-way L2, it stores 64 * 8 = 512 shadow lines. Full tags are a
simulation fidelity choice; no 0.23 KiB hardware-storage claim is made.

Counters start at zero (optimistic RRPV-2 insertion); the paper does not
specify an initial value. `initial_hysteresis` is a SimObject parameter.
Writebacks can allocate in this non-inclusive L2: they train as demand-side
accesses, independent of stale prefetch flags. Writeback hits do not promote.
Clean-evict, maintenance, store-training and uncacheable accesses do not train.

Both ordinary accesses (including misses) and locally dequeued prefetch
candidates train the sampler exactly once at lookup/issue, never again at
fill. Locally dropped cache-hit and MSHR-hit prefetch candidates are included;
this is the available XS prefetch request stream. Upstream `HardPFReq`
messages are first queued through the existing forwarding path, then observed
when dequeued, preventing duplicate notifications.

KPC-P's signature lookahead, dynamic fill-level threshold and path confidence
are outside the requested insertion-policy scope. The default XS composite
prefetcher has no universal path confidence. Consequently, prefetch hits do
not promote by default. `--kpc-promote-prefetch-hits` is an explicit experiment
that treats all admitted prefetches as confident. Prefetch depth and source
are never mislabeled as KPC-P confidence. Prefetch generation, throttling,
fill level, cache timing and capacity are unchanged by the policy selection.

## Configuration and comparison

Both `kmhv3.py` and `idealkmhv3.py`, with classic or sliced L2, accept:

```text
--l2-replacement=default|lru|xs-drrip|kpc
--kpc-sampled-sets=64
--kpc-hysteresis-bits=3
--kpc-promote-prefetch-hits
```

`default` preserves the configuration's existing policy. Explicit `lru` and
`xs-drrip` instantiate the original policies. All geometry is computed after
the final KMHV3 overrides, rather than assuming 1024 sets. New observation
hooks default to no-ops, preserving baseline behavior. The existing classic-L2
`slice_num` assignments are corrected to the actual `num_slices` parameter so
both classic configurations can instantiate.

For fair remote A/B/C comparisons, use the same tested commit, workload,
configuration and instruction budget, changing only `--l2-replacement`.
The first comparison uses workflow defaults (`idealkmhv3.py`,
`spec06-rva23-novec-gcc16-1.0c`, all benchmarks). Record exact run IDs and
coverage before interpreting scores. Track benchmark regressions alongside
L2 demand misses, memory traffic, insertion counts and sampler outcomes.

## Validation and results

The focused `kpc_sampler.test` covers saturation, first-use-only training,
allocation type preservation across demand/prefetch hits, used-victim
exclusion, true-LRU order, secure/full tags, deterministic set sampling and
shared counters with independent slice entries. Validation completed locally:

- `scons build/RISCV/gem5.opt --gold-linker -j48`: passed (GCC 13).
- `build/RISCV/mem/cache/replacement_policies/kpc_sampler.test.opt`: 9/9 passed.
- `tests/gem5/kpc/kpc_traffic.py`, with and without `--prefetch`: passed.
  The streaming case verifies saturated insertion; the prefetch case verifies
  issue observation and PF refill classification using the XS L2 composite.
- SPEC06 GCC16 `mcf/6753`, 100,000 committed instructions, NEMU difftest,
  embedded restorer, `DDR3_1600_8x8`: all seven cases passed. These cover
  KPC with `kmhv3.py` and `idealkmhv3.py`, each with classic and sliced L2,
  plus ideal sliced LRU, explicit XSDRRIP and unchanged default.
- Explicit XSDRRIP and default have identical `simTicks` and all 12138
  compared system statistics on that checkpoint.
- Repository style checks on changed regions, Python syntax and
  `git diff --check`: passed.

Commands and logs are retained in `/tmp/kpc-l2-01a1d0ba/validation/`;
`summary.json` records the cases and sampler statistics. The smoke memory
model differs from the remote DRAMsim3 performance configuration. These short
runs establish integration and functional correctness, not benchmark speedup.
Remote comparisons and subsequent iterations are recorded below.

Relevant stats are under `system.kpc_repl_state0` (one object per core) (accesses, sampler hit/miss,
first reuses, unused victims and DGH/PGH values) and each L2 replacement policy
(demand/prefetch RRPV-2/3 insertions and hit promotions).
