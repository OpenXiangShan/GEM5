# L1D hardware prefetch lookups on MainPipe

## Background

In XiangShan RTL, hardware prefetches originally used LoadPipe to check the
L1 data cache, competing with demand loads for lookup resources.
[RTL PR #5997](https://github.com/OpenXiangShan/XiangShan/pull/5997) moved
that lookup to the L1D MainPipe. GEM5 still charged both kinds of requests
against a common tag budget and waited for a memory-side send opportunity
before checking whether the prefetched line was already in L1. The value
of 100 in the Kunminghu V3 configuration was a per-cycle counter, not a
physical port count.

## Design

On the modeled L1D path, the fake MainPipe now handles each hardware
prefetch lookup. Refills, prefetches and StoreBuffer requests compete
for the same entrance in that priority order. A request can wait in a
shared one-entry buffer. The prefetcher keeps ownership of
a request until MainPipe accepts it. S1 records whether the line is
readable; S2 rechecks that result against the current classic-cache state.
A cache hit or an existing request for the line suppresses a duplicate
read. Only an eligible new miss allocates an MSHR for downstream issue.
An invalidated S1 hit or unavailable resource can cause the prefetch to
be dropped. This separates L1 lookup timing from downstream arbitration.

Demand loads continue to use LoadPipe. In Kunminghu V3, each of the three
scheduler-selected lanes can admit one new logical tag lookup per cycle,
subject to the shared tag-write conflict. Prefetches and demand loads on
these modeled paths no longer consume the old tag budget. That budget
remains available to the legacy paths.

In the default 16-entry setup, demand has no ID restriction, while
ordinary hardware prefetches can allocate IDs 2–15. A retiring miss keeps its ID reserved
until the existing fake refill completes. This prevents early reuse without
changing when refill data is installed or becomes readable. L2 prefetch
behavior and full RTL cycle-by-cycle equivalence are outside this design.

The O3 LSQ (`lsq.cc`) handles ingress, pipeline progression and load-lane
admission. The classic cache (`base.cc`) handles tag capture and miss
resolution. `mshr_queue.cc` enforces entry eligibility and refill holds.

## Validation and effect

Directed tests observed concurrent demand and prefetch lookups in 4,292
cycles and no demand tag read in the 5,652 checked tag-write cycles. With
four MSHRs, prefetches used only IDs 2 and 3 and never exceeded total
capacity. The tests also exercised readable tag results changing between
S1 and S2.

Before and after GEM5 runs used the same 38 checkpoints, the standard
Kunminghu V3 configuration, no warmup and five million instructions per
checkpoint. All passed difftest.

| GEM5 build | Geometric mean IPC |
| --- | ---: |
| Before: `111ca07f8b31894820b3c238aabd6fffb177ecce` | 2.871787 |
| After: `3553e9719e0837ec83e820684c227dd8b8df38fb` | 2.867151 |

IPC changed by **−0.161%** overall: 18 slices improved and 20 regressed.
The old tag budget caused no rejections in this suite. Moving the lookup
changed which prefetches issued and when they competed for MSHRs.
`legacy-lbm` (+0.549%) recorded more useful prefetches and fewer demand
read misses. `wrf_112496` (−2.218%) allocated more prefetch MSHRs but
recorded fewer useful prefetches and more cycles blocked by a full miss
queue. The directed tests confirm the new routing and contention rules.
The performance results show a small overall regression.
