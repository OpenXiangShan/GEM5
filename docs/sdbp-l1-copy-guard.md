# SDBP with L1-copy protection and a pure-LRU sampler

This experiment extends commit
`952aece4a7e1d7f13e742395edbb64e7b57d765f`, tested by Actions run
40811060728. It retains the L1-resident-copy filter and changes only sampler
victim selection from dead-first to pure LRU. Sampler size, predictor and
training rules, cache geometry, and workload arguments remain unchanged.

## Selection semantics

1. Use an invalid way first, as before.
2. Preserve the existing optional bypass behavior.
3. Scan predicted-dead candidates in their original way order. Select the first
   candidate absent from both the owning core's L1I and L1D resident tags.
4. If no dead candidate qualifies, use the original global LRU candidate.
   This fallback can evict a block with an L1 copy, including a predicted-dead
   block. Protection applies only to the dead-first selection stage.

There is no MRU restriction and no clearing of stored dead predictions.
The independent sampler uses invalid entries first, then the least recently
used valid entry, regardless of its stored dead prediction. Demand hits and
insertions refresh recency; prefetch hits retain their existing behavior and
do not refresh sampler metadata. Existing live/dead training rules are retained.
This keeps prediction-driven eviction from shortening the observation window
of a predicted-dead sampler entry.

## Residency model and scope

`configure_sdbp` wires each L2 (or each aligned L2 slice) to its owning CPU's
L1I and L1D through `BaseCache.sdbp_l1_caches`. `BaseCache` installs a read-only
callback in SDBP. The callback reconstructs the candidate's physical block
address with the L2 tag store, preserving the slice index, and queries the L1
tags with the candidate's secure/nonsecure attribute.

A valid resident line counts as a copy. MSHRs, queued responses, write buffers,
walker caches, and other CPUs' private caches do not count. The lookup does not
send coherence requests, consume simulated tag ports, modify LRU, train the
sampler, or copy data. This is an ideal residency-information experiment: the
latency and area of a hardware directory or probe network are not modeled.

The configuration rejects missing L1 caches. Nonempty probe targets are only
accepted on an L2 using SDBP and must have cache level 1. Standalone SDBP unit
fixtures without a callback retain the original selection behavior.

## Counters and validation

Each L2 SDBP policy reports:

- `l1CopyChecks`: dead candidates queried, stopping at the first eligible dead.
- `l1CopyProtectedDead`: queried dead candidates skipped due to L1 residency.
- `l1CopyLruFallbacks`: selections where all dead candidates were protected and
  the algorithm fell back to LRU. This is a subset of `lruVictims`.

Targeted policy tests cover an unprotected MRU, first-eligible candidate order,
all-dead-protected fallback including a protected LRU, changing residency,
unchanged dead bits, invalid-way priority, bypass, and absence of live probes.
The parent experiment passed checkpoint smoke tests for real L1 references.
This follow-up adds sampler tests for LRU with all entries predicted dead,
coexisting older-live and younger-dead entries, live training on reuse of a
retained dead entry, and unchanged prefetch-hit recency semantics. The existing
L1-copy policy tests are rerun with the modified sampler core. All 15
sampler/predictor tests and all 12 replacement-policy tests pass.

Remote performance evaluation uses the baseline `manual-perf.yml` inputs:
`idealkmhv3.py`, all `spec06-rva23-novec-gcc16-1.0c`, distributed servers
`default`, 32 jobs per server, and
`--no-pf --l2-replacement-policy=sdbp --sdbp-sampler-num=128 --sdbp-sampler-assoc=8`.
