# SDBP prediction evaluation under LRU

`--l2-replacement-policy=lru-sdbp` keeps the native `LRURP` timestamp
comparison and candidate-order tie breaking. SDBP predictions do not select
victims, bypass fills, protect L1 copies, or classify hierarchy writebacks.
The configuration applies independently to every L2 slice.

Each resident line stores a PC signature, `signatureValid`, prediction source
(demand, L1 prefetch, or L2 prefetch), and a dead prediction. The signature is
15 bits by default, rather than a single bit. Prefetch signatures use the
trigger PC, falling back to the ordinary request PC if the trigger PC is
unavailable. The predictor retains the existing SDBP hashes, counter updates,
threshold, and independent source banks. No independent sampler is allocated;
the complete real L2 residency supplies training outcomes, without partial-tag
aliasing or sampled-set restrictions. Predictor-counter aliasing remains.

## Outcome definitions

A prediction describes the interval after an eligible fill or access until the
next eligible access or committed replacement of that line:

| Observed outcome | Label | Training |
| --- | --- | --- |
| Demand or hardware-prefetch tag hit | Live | Update the stored signature live |
| Committed capacity/conflict replacement | Dead | Update the stored signature dead |
| Other invalidation, including flush/coherence/exclusive removal | Censored | No training |
| Still resident when statistics are dumped | Pending | No training |

Training and scoring use the **previous** stored signature and source. A hit
then installs the incoming signature and computes a new prediction after the
training update. Fill installs its first prediction after the old victim's
committed eviction. Victim selection and failed allocation attempts do not
train. A missing-PC demand/prefetch hit still demonstrates reuse and resolves
the old prediction, but cannot start a new one. Writebacks, instruction fetch,
PTW, maintenance, and software-prefetch requests do not resolve or replace
signatures. Their normal LRU timestamp effects are preserved.

Locally generated prefetches hitting a resident line are observed even though
they are dropped before `accessBlock()`. They train and update prediction
metadata without changing the LRU timestamp, matching native LRU behavior.
MSHR merges do not constitute resident-line reuse; demand merged into a
prefetch MSHR uses the existing demand-at-refill attribution.

**Dead therefore means no further eligible L2 access before replacement.** It
does not mean no future demand use anywhere in the hierarchy. In particular,
L1 hits are invisible to this experiment. Replacement is the LRU retention
horizon; a later miss to the same address does not relabel an earlier outcome.
Prefetch reuse counts as live even if demand never arrives.

## Statistics and interpretation

`fullCache*BySource` vectors use the source of the stored prediction, with
suffixes `l1Demand`, `l1Prefetch`, and `l2Prefetch`:

- `DeadTruePositive`: predicted dead, replaced before reuse.
- `DeadFalsePositive`: predicted dead, reused before replacement.
- `LiveTrueNegative`: predicted live, reused before replacement.
- `LiveFalseNegative`: predicted live, replaced before reuse.
- `DemandReuse` / `PrefetchReuse`: classify the resolving access independently
  of the stored source. `PredDeadDemandReuse` counts false-dead predictions
  directly exposed by demand.
- `Predictions`, `Resolved`, `Censored`, `Pending`, and `CarriedIn`: prediction
  lifecycle counts. `CarriedIn` records unresolved warmup predictions present
  at the last stats reset. Predictor and line metadata survive stats resets.

The separate prefetch-origin counters track a fill until its **first demand
use** or removal, even if intervening prefetch hits overwrite its signature:
`PrefetchFills`, `PrefetchDemandUses`, `PrefetchUnusedEvictions`,
`PrefetchUnusedInvalidations`, `PrefetchPending`, and `PrefetchCarriedIn`.
They measure prefetch demand-use coverage, not a demand-only predictor trained
with a different label.

Use `util/sdbp_observer_report.py` to aggregate slices and report source-specific
and combined-prefetch confusion matrices, accuracy, dead precision/recall,
live precision/recall, balanced accuracy, and an always-live baseline. Ratios
exclude pending and censored predictions; a zero denominator is JSON `null`.
The script checks lifecycle conservation, including predictions carried from
warmup. Report resolved and pending counts alongside accuracy to expose
measurement-window bias. These results evaluate full-cache-trained SDBP under
LRU, and do not measure the original small sampler or SDBP-driven replacement.

```sh
build/RISCV/gem5.opt --outdir=/tmp/sdbp-observer \
  configs/example/kmhv3.py --generic-rv-cpt=/path/to/slice.zstd \
  --l2-replacement-policy=lru-sdbp \
  --warmup-insts-no-switch=2000000 --maxinsts=5000000

# Repeat with identical workload/configuration and --l2-replacement-policy=lru.
python3 util/sdbp_observer_report.py /tmp/sdbp-observer/stats.txt \
  --baseline /tmp/sdbp-lru/stats.txt --output /tmp/sdbp-observer/report.json
```

The baseline comparison checks simulation ticks, instructions, and all numeric
`system.*` statistics except replacement-policy counters. Host timing is
excluded. Non-observer defaults remain unchanged. Sampler size/replacement
options have no effect in observer mode; demand-only sampling and bypass are
rejected. Predictor storage is unchanged; line state is O(L2 lines), hot-path
work is O(predictor tables), and stats-reset reconstruction is O(L2 lines).

## Validation

```sh
scons build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt \
  build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt -j16
build/RISCV/mem/cache/replacement_policies/sdbp_core.test.opt
build/RISCV/mem/cache/replacement_policies/sdbp_rp.test.opt
build/RISCV/gem5.opt tests/pyunit/util/pyunit_sdbp_config_check.py
```

## First checkpoint experiment: 2026-11-12

The local `milc/26907/_26907_0.036792_memory_.zstd` run uses `kmhv3.py`,
2 MiB L2 (four 512 KiB, eight-way slices), the default enabled prefetchers,
`DDR4_2400_16x4`, and the default SDBP predictor parameters. Both cases use the
same built executable and the repository-verified
`d30fff1ece9e-gem5-r3-multi16g-zfa-cbo/normal-dedup` reference, with memory
deduplication enabled. The requested window is 2,000,000 warmup instructions
followed by 3,000,000 measurement instructions; the final dump records
2,999,998 `simInsts`.

Both difftests complete successfully. All 11,122 compared non-policy numeric
statistics match native LRU exactly, including the final tick 1,381,624,326 and
the measurement duration 970,436,925 ticks. Independent sampler accesses,
SDBP dead-victim selections, and bypasses are all zero. The 30 predictor-core
tests, 33 policy-interface tests, three configuration tests, modified-region
style checks, and Python Black checks pass.

| Stored signature source | Resolved | Accuracy | Always-live accuracy | Dead precision | Dead recall |
| --- | ---: | ---: | ---: | ---: | ---: |
| Demand | 79,057 | 75.88% | 30.79% | 88.14% | 75.29% |
| L1 prefetch | 49,701 | 84.48% | 85.79% | 31.65% | 7.99% |
| L2 prefetch | 319,732 | 83.12% | 58.49% | 89.42% | 67.29% |
| Combined prefetch | 369,433 | 83.30% | 62.16% | 88.41% | 64.29% |
| All | 448,490 | 81.99% | 56.63% | 88.32% | 67.39% |

| Stored signature source | Dead TP | Dead FP | Live TN | Live FN |
| --- | ---: | ---: | ---: | ---: |
| Demand | 41,196 | 5,544 | 18,796 | 13,521 |
| L1 prefetch | 564 | 1,218 | 41,421 | 6,498 |
| L2 prefetch | 89,302 | 10,566 | 176,450 | 43,414 |

There are 52,799 demand reuse hits with a valid stored prediction. Of these,
5,597 (10.60%) encounter a dead prediction. Almost all such hits resolve a
prefetch-source signature; source-grouped accuracy must not be interpreted as
accuracy conditioned on an incoming demand. The combined prefetch-origin
first-demand-use rate is 52,798 / (52,798 + 115,450) = 31.38% among resolved
usefulness samples. This is distinct from the 83.30% prefetch-source accuracy,
which also rewards correct predictions of subsequent prefetch reuse.

The window ends with 22,435 unresolved predictions (4.76% of started plus
carried-in predictions), and no censored prediction in this particular run.
L1-prefetch dead recall is low and its accuracy falls below an always-live
baseline. The demand and L2-prefetch results improve on that baseline for
this slice; one short slice does not establish workload-wide accuracy.

Exact commands, configuration files, both stats dumps, the source diff, binary
and reference hashes, and the machine-readable report are saved under
`/tmp/sdbp-full-cache-observer-20261112-pinned-ref/`. `report.json` contains
per-slice and aggregate results plus the LRU comparison. The earlier default
reference attempt failed in both cases on the restorer's `mstateen0` CSR
before measurement; it is excluded from these results.
