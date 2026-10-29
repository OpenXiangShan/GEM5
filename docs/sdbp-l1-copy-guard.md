# SDBP dead-victim selection with L1-copy protection

This experiment starts from commit
`16878a2bca0791bec7d49df58ec7c831286868cb`, the baseline of Actions run
40486816390. It replaces the separate MRU-guard experiment with a resident-copy
filter. Sampler and predictor behavior, cache geometry, and workload arguments
remain unchanged.

## Selection semantics

1. Use an invalid way first, as before.
2. Preserve the existing optional bypass behavior.
3. Scan predicted-dead candidates in their original way order. Select the first
   candidate absent from both the owning core's L1I and L1D resident tags.
4. If no dead candidate qualifies, use the original global LRU candidate.
   This fallback can evict a block with an L1 copy, including a predicted-dead
   block. Protection applies only to the dead-first selection stage.

There is no MRU restriction and no clearing of stored dead predictions.
The independent sampler still uses its original replacement and training rules.

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
A checkpoint smoke test additionally exercises real L1 references and the
aligned L2 address reconstruction with difftest enabled. The 12 policy tests
and 13 predictor-core tests pass. A two-million-instruction smoke run uses the
normal threshold 8. A separate four-million-instruction integration diagnostic
uses threshold 0 solely to force the query path: it checks 197 dead candidates
and skips 83 resident L1 copies, with difftest passing. The remote performance
run keeps threshold 8; the forced-dead diagnostic is not a performance result.

Remote performance evaluation uses the baseline `manual-perf.yml` inputs:
`idealkmhv3.py`, all `spec06-rva23-novec-gcc16-1.0c`, distributed servers
`default`, 32 jobs per server, and
`--no-pf --l2-replacement-policy=sdbp --sdbp-sampler-num=128 --sdbp-sampler-assoc=8`.
