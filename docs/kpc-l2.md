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
