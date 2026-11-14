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


## Local screening: initial implementation 60f9ac4ec2

These are single highest-weight checkpoints, **not** a full SPEC06 score.
Each uses the same `idealkmhv3.py`, DDR3 memory model, 2M warmup and about 2M
measured instructions. NEMU difftest passed in all 12 runs. The three policy
baselines share the same executable; the promotion experiment changes only
`--kpc-promote-prefetch-hits`.

| Checkpoint | LRU IPC | XSDRRIP IPC | KPC v1 IPC | KPC / LRU | KPC / XSDRRIP |
| --- | ---: | ---: | ---: | ---: | ---: |
| mcf/6753 | 0.696185 | 0.706001 | 0.695864 | -0.046% | -1.436% |
| lbm/6382 | 2.045235 | 2.056141 | 2.055693 | +0.511% | -0.022% |
| libquantum/69175 | 2.423003 | 2.405900 | 2.423003 | 0.000% | +0.711% |

Always promoting prefetch hits changes KPC IPC by +0.095%, -0.433%, and
-0.036% on these respective points. This does not resolve the mcf regression
against XSDRRIP, so the conservative default is retained for now. These
results do not establish that optimization opportunities are exhausted.

For mcf, v1 reduces the reported L2 `demandMisses::total` from 116023 to
115063 relative to XSDRRIP, but increases L3 demand misses from 42793 to
43824 and L1D demand misses from 384560 to 392939. The runtime prefetch
feedback also changes. Thus, L2 misses alone do not explain total performance.
The cache stats aggregate by MemCmd: downstream prefetches converted to
`ReadReq` can be included in the reported demand counts.

Artifacts: `/tmp/kpc-l2-01a1d0ba/local-screen-v1-summary.json`,
`local-compare-v1/`, and `local-compare-promote/` in the same task directory.
The original executable and SHA256 are retained under `versions/` and
`v1-binary.json` for reproducing subsequent policy comparisons.

## Clean-writeback sampling correction

Source inspection found that `Packet::isCleanEviction()` includes both
no-data `CleanEvict` and data-carrying `WritebackClean`. V1 therefore skipped
clean writebacks despite the intended demand-side writeback training above.
The observer now excludes only `MemCmd::CleanEvict`, allowing both clean and
dirty writebacks to train. Actual writeback hits still do not promote RRIP.

The two-level `kpc_traffic.py --clean-writebacks` regression creates L1 clean
writebacks and compares the observer count against actual L2 command-access
statistics. The saved v1 executable reports 1088 observed accesses versus
2174 expected (1088 reads plus 1086 clean writebacks), demonstrating the gap.
The corrected implementation observes all 2174 accesses and 1029 first reuses.
Demand-only and prefetch traffic tests and all nine sampler unit tests also
pass after the correction. The performance comparison follows below.


The corrected policy is committed as `3dab7453fa`. Classic and sliced L2
`mcf/6753` checkpoint smoke tests both pass NEMU difftest after the fix.
All three 2M-warmup/2M-ROI local screening points also pass difftest:

| Checkpoint | KPC corrected IPC | vs LRU | vs XSDRRIP | vs KPC v1 |
| --- | ---: | ---: | ---: | ---: |
| mcf/6753 | 0.704443 | +1.186% | -0.221% | +1.233% |
| lbm/6382 | 2.055693 | +0.511% | -0.022% | +0.000% |
| libquantum/69175 | 2.423003 | +0.000% | +0.711% | +0.000% |

The baseline policy implementations and all workload/configuration arguments
are unchanged; only the KPC access observer differs between these commits.
The comparison manifest is `local-screen-v2-summary.json`; complete outputs
are in `local-compare-v2/`, under `/tmp/kpc-l2-01a1d0ba/`.

This correction reduces the local mcf regression but does not establish a full
SPEC06 improvement or exhaust optimization opportunities. Keep the original
KPC-R RRPV 2/3 insertion mapping and conservative prefetch promotion pending
longer, complete-suite evidence. If full runs confirm regressions, a separate
L2-specific live-line insertion experiment is a candidate; it has not been
implemented or claimed as an improvement.

## Remote SPEC06 comparison

The user explicitly authorized publishing `60f9ac4ec2` and dispatching the
three complete SPEC06 comparisons. The exact initial commit
`60f9ac4ec2afce93df9120d62b238509f321ad40` was pushed to
`OpenXiangShan/GEM5:kpc-l2`; subsequent local corrections are not part of
this first remote comparison.

All three runs use `manual-perf.yml` at workflow commit
`24be168ad2ac878d3c6ac7b19aa6857fb07ef00a` (`xs-dev`),
`idealkmhv3.py`, `spec06-rva23-novec-gcc16-1.0c`, all benchmarks,
`distributed_servers=default`, and 32 jobs per server. The tested SHA is
supplied explicitly through the workflow `branch` input. Only
`--l2-replacement` changes.

| Policy | Run | Initial status |
| --- | --- | --- |
| lru | [43469074117](https://github.com/OpenXiangShan/GEM5/actions/runs/43469074117) | in_progress |
| xs-drrip | [43469084627](https://github.com/OpenXiangShan/GEM5/actions/runs/43469084627) | in_progress |
| kpc | [43469091526](https://github.com/OpenXiangShan/GEM5/actions/runs/43469091526) | queued |

These runs have been created, not declared passed. The workflow head SHA is
not the tested code SHA. Inspect the build checkout, completion status,
actual archive path and coverage before interpreting the scores. The known
clean-writeback sampling correction `3dab7453fa` remains validated locally
and must be evaluated separately from this initial comparison.

Dispatch commands and run metadata are retained in
`/nfs/home/chenzhuo/.tmp/kpc-l2-remote-20261114/`.

### Completed KPC and XSDRRIP pair; LRU pending

Both policies completed all 1094 checkpoints with no abort markers. All
29 benchmarks have reported coverage 1.0. The performance result checks
passed. Both final Actions conclusions are `failure`: the subsequent archive
retention cleanup encountered NFS `Stale file handle` errors while deleting
older run directories (including `20261028_211147_2174b82_idealkmhv3_run118`).
Current-run data and scores remain readable, and the score artifact upload
succeeded. This is an infrastructure cleanup failure, not a simulation or
performance-check failure. No simulations were rerun for this reason.
The LRU run remains in progress, so this is the complete KPC/XSDRRIP pair,
not the final three-policy comparison.

| Estimated 3.0 GHz score | XSDRRIP | KPC | KPC change |
| --- | ---: | ---: | ---: |
| Int | 65.734135 | 65.730046 | -0.006% |
| FP | 70.960019 | 71.113644 | +0.216% |
| overall | 68.748997 | 68.834436 | +0.124% |

The largest KPC score regressions against XSDRRIP are gcc (-0.93%),
libquantum (-0.40%) and mcf (-0.22%). The largest gains include soplex
(+0.91%), sphinx3 (+0.75%) and zeusmp (+0.48%). Benchmark deltas use
the score file's rounded benchmark values. Do not mix the historical LRU
run on `a61859f355` into this same-commit comparison.

KPC archive:
`/nfs/home/share/gem5_ci/performance_data/spec06-rva23-novec-gcc16-1.0c/20261114_011153_60f9ac4e_idealkmhv3_run2144`

XSDRRIP archive:
`/nfs/home/share/gem5_ci/performance_data/spec06-rva23-novec-gcc16-1.0c/20261114_011036_60f9ac4e_idealkmhv3_run2143`

Machine-readable results are `complete-scores.json` and
`kpc-vs-xsdrrip.json` in the dispatch artifact directory above.

### Complete same-commit three-policy comparison

All three runs completed all 1094 checkpoints; all 29 benchmarks report
coverage 1.0. The LRU Actions conclusion is `success`; KPC and XSDRRIP
passed simulation and result validation but failed old-archive NFS cleanup,
as documented above. All three test commit
`60f9ac4ec2afce93df9120d62b238509f321ad40`.

| Estimated 3.0 GHz score | LRU | XSDRRIP | KPC | KPC / LRU | KPC / XSDRRIP |
| --- | ---: | ---: | ---: | ---: | ---: |
| Int | 65.691192 | 65.734135 | 65.730046 | +0.059% | -0.006% |
| FP | 71.266931 | 70.960019 | 71.113644 | -0.215% | +0.216% |
| overall | 68.904515 | 68.748997 | 68.834436 | -0.102% | +0.124% |

KPC improves over XSDRRIP overall, but does not beat LRU overall. The
largest regression against LRU is milc (-4.40%); gcc also regresses
(-0.50% vs LRU, -0.93% vs XSDRRIP). Investigate these points and the
previously identified clean-writeback correction before declaring the
replacement policy optimized.

LRU archive:
`/nfs/home/share/gem5_ci/performance_data/spec06-rva23-novec-gcc16-1.0c/20261114_011445_60f9ac4ec2_idealkmhv3_run2142`

Machine-readable results: `three-policy-scores.json` in the dispatch
artifact directory.

### Focused investigation and optional insertion experiment

Reprocessing all three archives reproduced their scores exactly. Raw CSVs
contain 1094 points each. The local processor initially lacked SciPy; an
isolated task dependency resolved it. Raw/weighted/score CSVs and processor
logs are retained under the dispatch artifact directory's `proc/`.

The largest time contributions identify `milc/5841`, `milc/9643`,
`gcc_g23/6319`, `gcc_166/1439`, `mcf/6753`, and `libquantum/69175`
for targeted screening. At `gcc_g23/6319`, KPC IPC is 15.60% below
XSDRRIP and L3 demand misses increase from 34929 to 74974. At
`gcc_166/1439`, L3 demand misses increase from 80320 to 213728.
Milc's benchmark cycles grow 4.60% against LRU while its L2 load misses
fall about 1.04%, so miss count alone does not explain the runtime change.

The previously committed clean-writeback correction is being screened with
CI's DRAMsim3 DDR4 configuration, pinned normal-dedup REF, 20M warmup and
20M measured instructions. A LRU control point checks local/CI comparability.
Prefetch-hit promotion is evaluated separately from the insertion experiment.
Saved executables and per-run commands prevent a concurrent rebuild from
changing queued experiments.

`KPCReplState.live_rrpv` is an optional L2 experiment: when DGH/PGH is
not saturated, insert at this RRPV (0, 1 or 2); saturation still inserts
at 3. The default remains the paper's value 2. Sampler training, hysteresis,
victim selection and demand promotion are unchanged. For example:

```text
--l2-replacement=kpc --param=system.kpc_repl_state0.live_rrpv=1
```

A more protected insertion may preserve useful L2 lines but can also retain
pollution longer. No performance improvement is claimed for this option
until the targeted and broader comparisons complete. RRPV 0/1 insertion
counts are reported separately for demand and prefetch fills.

Validation: the DRAMsim3 build, RRPV 0/1 FS smoke cases with NEMU difftest,
invalid-parameter rejection, all nine sampler unit tests, Python syntax,
changed-region style and diff whitespace checks pass. Default RRPV 2 matches
all common system statistics and simulation ticks against the previous
corrected executable in the smoke case. Full evidence is recorded in
`live-rrpv-validation.json`, `live-smoke-results.json`,
`screen-ci-model-manifest.json` and `screen-live-rrpv-manifest.json`.
The configuration and functional checks above preceded the completed
long-window screening below.


### Completed long-window insertion screening

All 19 local cases exited normally after 20M warmup and 20M measured
instructions, using the CI DRAMsim3 DDR4 model and pinned dedup REF. The
`milc/5841` LRU control reproduces all 12,875 common system, instruction
and tick statistics from CI exactly, including IPC 1.053290. Each comparison
uses the final complete statistics block. Binary hashes, native parameters,
commands and ROI length were checked; no case timed out.

The corrected KPC column includes clean-writeback sampler training and
retains insertion RRPV 2. The prefetch-promotion column changes only the
existing hit-promotion option. RRPV 0/1 columns retain normal hit behavior
and change only non-saturated insertion. Saturated insertion remains RRPV 3.

| Checkpoint | LRU CI | XSDRRIP CI | Initial KPC CI | Corrected KPC | PF promotion | RRPV 1 | RRPV 0 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| milc_5841 | 1.053290 | 0.963310 | 0.966141 | 0.962484 | 0.968544 | 0.985865 | 1.044229 |
| milc_9643 | 1.126472 | 0.990995 | 0.986893 | 0.986893 | 0.998819 | — | — |
| gcc_g23_6319 | 0.770037 | 0.885293 | 0.747225 | 0.749994 | 0.749375 | 0.864466 | 0.860348 |
| gcc_166_1439 | 0.880696 | 0.870339 | 0.783925 | 0.785341 | 0.705889 | 0.785682 | 0.790932 |
| mcf_6753 | 0.746060 | 0.737773 | 0.734756 | 0.734250 | 0.734676 | — | — |
| libquantum_69175 | 5.414300 | 5.478076 | 5.421564 | 5.471455 | 5.471420 | — | — |

The clean-writeback correction is required for sampler semantics, but is not
a uniform performance improvement. It improves `libquantum/69175` by 0.92%
against initial KPC while slightly worsening `milc/5841` and `mcf/6753`.
Prefetch-hit promotion is rejected as a default optimization: it reduces
`gcc_166/1439` IPC from 0.785341 to 0.705889 despite small milc improvements.

Non-saturated RRPV 0 improves `milc/5841` IPC by 8.493% against corrected
KPC and reduces the gap to LRU to 0.86%. It improves `gcc_g23/6319` by
14.714% against corrected KPC, but remains 2.82% below XSDRRIP. At
`gcc_166/1439`, RRPV 0 still trails LRU by 10.19%. This parameter is
therefore a candidate for broader validation, not a complete regression fix.

The original manifests record base HEAD `e401274c41`. The live-RRPV
executable was frozen with the then-uncommitted parameter implementation,
subsequently committed as `daf86dbe4b`; its captured hash and observed
RRPV 0/1 configuration distinguish it from the earlier corrected executable.
`live-rrpv-implementation.patch` preserves the implementation difference.
The default RRPV 2 implementation remains unchanged.

Machine-readable evidence is `focused-screen-results.json`, with original
case directories under `screen-ci-model/` and `screen-live-rrpv/` in the
task artifact directory. These selected points do not establish a new
full-suite score. Expanded validation will cover the remaining regression
points and representative original gains before choosing a new default.

Expanded screening uses the same frozen live-RRPV executable for 13 cases:
RRPV 0 on `milc/9643`, `mcf/6753`, and `libquantum/69175`; RRPV 0/2
pairs on `lbm/6382`, `soplex_ref/5312`, `sphinx3/157921`, `zeusmp/27787`,
and `leslie3d/18494`. The first four added benchmarks select the largest
weighted gain against XSDRRIP, and leslie3d selects the largest weighted
regression against LRU. The comparison retains 20M warmup and 20M ROI,
six workers, no prefetch-hit promotion, and a two-hour per-case timeout.
Its manifest, selection rationale and outputs are under `screen-expanded`.
The paper default remains RRPV 2 while this evaluation is pending.

### Completed expanded screening

All 13 expanded cases passed NEMU difftest, normal instruction-limit exit,
final complete 20M ROI, frozen-executable hash and native-parameter checks.
The runner's first attempt failed during argument parsing before simulation;
those logs are retained under `screen-expanded-argparse-failure/`. The
corrected runner reuses the previously validated command template, including
memory deduplication and the exact warmup option. The successful results are
separate and are recorded in `expanded-screen-results.json`.

| Checkpoint | Corrected KPC RRPV 2 | RRPV 0 | IPC change |
| --- | ---: | ---: | ---: |
| milc_9643 | 0.986893 | 1.128169 | +14.315% |
| mcf_6753 | 0.734250 | 0.736044 | +0.244% |
| libquantum_69175 | 5.471455 | 5.459276 | -0.223% |
| lbm_6382 | 2.593846 | 2.566000 | -1.074% |
| soplex_ref_5312 | 2.759998 | 2.755375 | -0.168% |
| sphinx3_157921 | 4.333400 | 4.303279 | -0.695% |
| zeusmp_27787 | 3.504813 | 3.495314 | -0.271% |
| leslie3d_18494 | 5.300985 | 5.349164 | +0.909% |

RRPV 0 removes the large `milc/9643` regression (IPC 1.128169 versus LRU
1.126472), but does not uniformly preserve the original gains. In these
selected points it loses 1.074% on lbm and 0.695% on sphinx3 relative to
corrected RRPV 2. The default remains RRPV 2. Neither the selected-point
results nor a partial substitution into the original score establish a
full-suite gain.

### Writeback-hit investigation

The remaining `gcc_166/1439` regression motivates a separate L2 experiment.
`BaseSetAssoc::accessBlock` calls replacement `touch` only on a real tag
hit. Current KPC ignores writeback hits; XSDRRIP gives non-prefetch release
reuse RRPV 0. The benchmark has substantial clean-writeback traffic. Cache
`Writeback*.hits` statistics include newly allocated writebacks because
`BaseCache::access` records them as satisfied hits; they are not proof of
actual tag-hit count. A replacement-policy counter must measure that event.

The optional `KPCRP.promote_writeback_hits` experiment promotes data-bearing
writeback tag hits to RRPV 0. It excludes no-data CleanEvict and preserves
sampler training, insertion, prefetch-hit behavior and victim selection.
It adds one scalar counter and constant work per affected hit. The default
is false. Promoting a released line can preserve later demand reuse, but
can also extend the lifetime of streaming stores. Validation will first
check default-stat equivalence and NEMU smoke, then compare focused points
with and without the option before selecting any broader candidate.

The DRAMsim3 build, modified-region style, Python syntax and whitespace
checks passed. All five 100K-instruction NEMU smoke cases passed. With
writeback promotion disabled, RRPV 2 reproduces all 11,686 common system,
inst and tick statistics, and RRPV 0 reproduces all 11,685 common values,
against the previous executable. With the option enabled, sliced milc,
sliced mcf and classic mcf report 92, 10,306 and 10,308 actual writeback
promotions respectively. Evidence is in `writeback-promotion-validation.json`
and `writeback-smoke-results.json`.

The sliced-L2 native parameter is:

```text
--param=system.l2_wrappers[0].slices[:].inner_cache.replacement_policy.promote_writeback_hits=True
```

The six planned long-window cases use writeback promotion on: RRPV 0/2
at `gcc_166/1439`, plus RRPV 0 at `gcc_g23/6319`, `milc/5841`, `mcf/6753`
and `lbm/6382`. They compare with the already completed matching cases
without promotion, with the same DRAMsim3, REF and 20M/20M instruction
windows. A separate frozen executable protects provenance. No performance
claim is made for this option until those results finish.

### Writeback-hit results and candidate selection

All six writeback-promotion cases passed NEMU difftest and final complete
20M ROI checks. All four slices had the requested parameter enabled;
recorded actual tag-hit promotions were nonzero, and executable hashes and
committed source hashes were verified. Comparison uses the matching earlier
case with promotion disabled and the same insertion parameter.

| Checkpoint | Insertion RRPV | WB promotion off IPC | WB promotion on IPC | IPC change | Actual promotions |
| --- | ---: | ---: | ---: | ---: | ---: |
| gcc_166_1439 | 2 | 0.785341 | 0.787731 | +0.304% | 207561 |
| gcc_166_1439 | 0 | 0.790932 | 0.784769 | -0.779% | 205137 |
| gcc_g23_6319 | 0 | 0.860348 | 0.848886 | -1.332% | 115855 |
| milc_5841 | 0 | 1.044229 | 1.042441 | -0.171% | 596628 |
| mcf_6753 | 0 | 0.736044 | 0.735140 | -0.123% | 5362406 |
| lbm_6382 | 0 | 2.566000 | 2.566805 | +0.031% | 1184186 |

Writeback-hit promotion does not explain the large gcc_166 regression.
It improves the RRPV-2 case by only 0.304% and worsens the RRPV-0 case
by 0.779%; gcc_g23 loses 1.332%. With RRPV 0 it also slightly worsens
milc and mcf. The optional promotion parameter, branch and counter are
removed from the candidate source. Commit `62fe4e89d1` and
`writeback-promotion-implementation.patch` retain the tested experiment;
`writeback-screen-results.json` retains all observations. The earlier
writeback section describes that historical experiment, not a currently
available parameter.

The selected broader-validation candidate is corrected clean-writeback
sampler training plus non-saturated insertion RRPV 0, with existing demand
hit promotion and no prefetch/writeback-hit promotion. Saturated insertion
remains RRPV 3. RRPV 0 is selected for complete-suite evaluation because it
materially improves the two dominant milc regressions and gcc_g23, while
its observed losses on representative gains are smaller. This is a local
screening judgment, not a full-suite improvement claim. The implementation
and compiled default remain RRPV 2 pending broader evidence.

The proposed next comparison uses one new exact code SHA and two complete
SPEC06 runs: corrected KPC at RRPV 2, and the same code at RRPV 0. This
separates the semantic clean-writeback fix from insertion tuning. Existing
LRU/XSDRRIP baselines retain 1094/1094 coverage, and their code and all
configuration/observation-hook code have no differences from the initial
tested commit. The local LRU control already reproduces every common
performance statistic. There is no reason to repeat the original three
runs. A proposed publication and exact dispatch-input record will be
prepared separately; no later commit or additional full-suite dispatch
has been published under the initial 60f9ac4ec2-only authorization.
