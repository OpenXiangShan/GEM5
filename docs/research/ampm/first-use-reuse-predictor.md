# Optional reuse prediction after first prefetch consumption

`--ampm-first-use-predictor` enables an experimental extension to PAMAIP.
It defaults to false and currently requires `--ampm-hint-source=cache`.
The target is another demand access after first consumption of a prefetched
line. An unused prefetch is not a negative example for this target: it has
never had the first consumption that defines the prediction.

## Replacement behavior

At the first demand consumption of a prefetched line, including a late demand
served directly by its prefetch fill:

- Confident reuse: set RRPV to zero.
- Confident no-reuse: set RRPV to the maximum, making it eligible for eviction.
- Unknown: preserve the existing map/line policy.

Later hits retain the original behavior. In the fixed SRRIP3/map experiment,
insertions still use RRPV 6; a confident no-reuse prediction uses RRPV 7.
No extra per-line field is required in the actual cache: the existing
`unusedPrefetch` state identifies first consumption.

## Predictor and feedback

One predictor belongs to the L2 AMPM hint table and is shared by its slices.
A physical 4 KiB region and security-domain hash indexes 1024 untagged entries.
Each entry logically holds a 2-bit saturating reuse tendency, initialized to
1, and a 2-bit saturating sample count. Three outcomes are required before
prediction; only endpoint tendencies 0 and 3 issue confident predictions.
Conflicting outcomes return the entry to unknown. Hash aliasing is possible.

An independent 128-entry, four-way LRU sampler observes one in 256 hashed
64-byte block addresses. All accepted cacheable L2 requests are eligible;
requests carry the existing insertion-hint deduplication, so a forwarded
request is observed once. Evictions, writebacks and maintenance are excluded.
Prefetch hits do not count as demand uses or restart a sampled lifetime.

A sample originating with a prefetch trains positive on its second demand
access, once per sampled lifetime. Retirement after exactly one demand trains
negative. Retirement with no demand increments an unused-sample counter only.
Demand-origin lifetimes never train this predictor. The prediction snapshot
at the sampled first demand is evaluated against that sampled outcome.
Samples include real-cache misses, allowing later demand to correct a
prediction even after the actual line was evicted. Negative training is
therefore independent of the policy's actual victim choices.

## Model limits and diagnostics

The sampler tracks request lifetimes, not confirmed prefetch fills. Its finite
LRU lifetime defines the observation horizon; it cannot prove that a line
will never be used again. Demands hidden by L1 do not appear in the L2 stream.
The untagged region table can alias, and workload phase changes can invalidate
history. This extension predicts post-consumption reuse rather than zero-use
prefetch accuracy.

The model adds bounded hash work and at most a four-way sampler lookup per
sampled request. It does not model extra SRAM ports, critical-path delay,
arbitration, area or power. C++ container sizes are not RTL area estimates.

Shared statistics report sampled accesses, positive/negative training,
unused samples, and correct/wrong confident sampled predictions. Per-slice
`firstUsePredictHigh`, `firstUsePredictLow` and `firstUsePredictUnknown`
report actual first-consumption decisions. Sampled prediction correctness
must not be interpreted as exact real-cache dead-prefetch precision.

The table parameters are `first_use_entries=1024`,
`first_use_sample_entries=128`, and `first_use_sample_period=256` on
`AMPMHintTable`. They can be changed through gem5 `--param` for experiments.

## Validation and full-run configuration

The policy unit test includes cold/confidence behavior, single-use and
second-use training, unused-prefetch exclusion, demand-origin exclusion,
prefetch-hit handling, independent miss feedback, security-domain lifetimes,
LRU refresh, no double training, correction, and counter reversal.
Integration checks compare common non-host statistics with the pinned
original binary when the switch is off, then exercise enabled prediction
with difftest and inspect effective configuration and decision counters.

The full experiment retains idealkmhv3 timing and cache geometry, uses fixed
SRRIP3/map/cache hints, and adds only `--ampm-first-use-predictor` to the
previous control configuration. It uses all SPEC06 gcc16 RVA23 no-vector
1.0c points, 20M warmup / 40M total, mem-dedup and the pinned normal-dedup REF.
Berti, Temporal, L1 LLDP, L2 LLDP, CDP and Despacito Stream remain disabled;
the L3 Worker helper remains present. Performance reports stay local.
