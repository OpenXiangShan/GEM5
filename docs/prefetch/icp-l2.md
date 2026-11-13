# L2 feedback-directed insertion experiments

## Scope and sources

Implement the insertion/accuracy-prediction part of ICP (ICP-AP) in an L2
replacement policy called ICPRP. This is not the complete HPCA 2007 FDP
prefetcher controller, nor a claim to reproduce its results. Prefetch generation,
cache sizes, and timing stay under the normal configuration.

- CRP, section 5.4.1 (PDF page 67), identifies ICP and FDP.
- Vivek Seshadri et al., *Mitigating Prefetcher-Caused Pollution using Informed
  Caching Policies for Prefetched Blocks*, TACO 11(4), article 51, January 2015.
  DOI: 10.1145/2677956. Author copy:
  https://users.ece.cmu.edu/~omutlu/pub/informed-caching-for-prefetching_taco15.pdf
- Santhosh Srinath et al., *Feedback Directed Prefetching: Improving the Performance
  and Bandwidth-Efficiency of Hardware Prefetchers*, HPCA 2007.
  DOI: 10.1109/HPCA.2007.346185.

Local paper copies are in `docs/prefetch/papers/`. The authors' project page and SAFARI tools page both link
`https://github.com/CMU-SAFARI/memsim`. This repository was cloned to
the pinned revision `45e57250c9b60f29243c9c826ec7d9bf9bd75051`.
`Simulator/CmpDCP.h` implements ICP: accuracy lookup/insertion at lines 634–648,
useful credit at lines 416–420, EPF miss credit at lines 483–492, and unused
prefetch/EPF displacement training at lines 706–718. `CmpFDPAP.h` is the separate
FDP comparison. The gem5 code is written from these mechanisms without copying
third-party source. The paper's per-stream predictor becomes per-source here.

## Model

ICPRP keeps an explicit LRU rank per line and a 4-bit saturating accuracy counter
per prefetch source and L2 slice. An accurate prefetch enters at MRU (configurable
rank); an inaccurate prefetch enters at LRU. A first demand hit trains success.
An unused accurate prefetch trains failure on invalidation. An unused inaccurate
prefetch enters a bounded, four-way evicted-prefetch filter (EPF): a subsequent
demand miss trains success; displacement without use trains failure. An address
is counted at most once per residency. A new residency retires an unresolved
EPF record without classifying it as inaccurate; only a true EPF displacement
trains a failure. `epfSuperseded` records these neutral retirements. EPF entries include full physical line
addresses and the secure bit; unlike the paper's partial tags this has no tag
alias false positives. The current composite prefetchers expose source IDs but
not a uniform stream ID, so source-specific counters replace paper stream IDs.

On a prefetch tag hit, the incoming source/PC accuracy controls MRU
promotion independently of the insertion rank. The author source exposes
this choice through `use-accuracy-prefetch-hit`; its one-core
`1m-ap-only-aph` configuration enables it, while `1m-ap-only` leaves that
option disabled. The gem5 experiment therefore includes this explicit
hit-promotion choice as well as accuracy-based insertion.
`--icp-promote-all-prefetch-hits` disables the accuracy check on prefetch hits,
while retaining accuracy-based insertion. It selects the insertion-only hit
behavior of `1m-ap-only`; leaving it unset retains the `1m-ap-only-aph` choice.
The composite prefetchers and accuracy indexing still differ from the author
model, so this does not reproduce every author configuration.

Demand hits normally promote to MRU. `--icp-demote-on-first-use` separately enables
ICP-D's first-use behavior. A fill-completion hook also credits a demand
merged into an outstanding hardware prefetch exactly once. Training-only requests
are excluded. Neither EPF nor demand-use credit changes prefetch issue behavior.

Rank changes cost O(associativity), victim selection O(associativity), and EPF
lookups O(EPF associativity). Filter storage and predictor storage are bounded.
Coherence invalidations also terminate residency and train unused prefetches.

## Validation and experiment plan

1. Build gem5 and test stack ordering, useful/unused credit, EPF correction,
   secure-address separation, source isolation, and duplicate-use suppression.
2. Smoke-test real cache hierarchy and inspect effective config.json and stats.
3. Commit and push the experimental branch. Dispatch matched LRU, XSDRRIP and
   ICPRP runs with gh/manual-perf, holding configuration, suite, warmup and
   measurement lengths fixed. Record run IDs, tested SHA, arguments and coverage.
4. Inspect regressions with L2 hit/miss and policy feedback statistics. Change
   replacement behavior where evidence supports it, repeat affected workloads,
   and confirm final results with full matched coverage.

## Current state

Experimental branch: `icp-l2`, based on `f9649a04ee`. The retained implementation
includes the prefetch-hit recency correction at `6f1545f6c6`; unsuccessful
prefetch-read-credit experiments have been removed. The best completed
145-point candidate is PC64 with low rank 4: -0.0627% versus LRU and
+0.2557% versus XSDRRIP. PC64 with low rank 1 also completed all 145 points:
-0.0637% versus LRU and +0.2546% versus XSDRRIP. Neither static setting
improves over both references. These are local ICP results; an ICP Actions
run and full-profile confirmation remain pending.

The optimized simulator builds successfully. Python syntax, repository style
checks and `git diff --check` pass. Local CoreMark smoke runs for LRU, XSDRRIP
and ICP all pass difftest (20k warmup, 200k total instructions) and finish at
exactly tick 48,449,502. Their effective system configurations are identical
after removing replacement-policy parameters. This small program is a
functional/configuration check, not a performance comparison.

A real mcf checkpoint (`mcf/8145/_8145_0.038182_memory_.zstd`) passes difftest
with 200k warmup / 1M total instructions. The 800,001-instruction measured
region records 52,227 L2 prefetch fills, 22,132 high-priority and 30,095
low-priority insertions, 25,764 useful prefetches, and 713 EPF corrections across
four slices. These counts demonstrate the actual hardware-prefetch
feedback path; they do not establish a speedup.

The deterministic cache tests run with:

```sh
build/RISCV/gem5.opt -d "$HOME/.tmp/icp-feedback" tests/gem5/replacement_policies/icp_trace.py --scenario feedback
```

Other scenarios are `no-epf`, `secure`, `demotion`, `merged`, and `superseded`. Hardware-prefetch
trace packets need XiangShan source metadata; TraceGen initializes default
metadata for such packets so the tests traverse the existing prefetch queue.
The first four scenarios were also checked through software-prefetch fills.

Matched screening references use `idealkmhv3.py`,
`spec06-rva23-novec-gcc16-0.3c`, all 145 points, and explicit
`--warmup-insts-no-switch=20000000 --maxinsts=40000000`. The references ran
through Actions on the default distributed pool with 16 jobs/server.
The ICP screens below run locally with 16 workers and identical point lists,
weights, configuration, and REF. Full 1.0c candidate validation remains pending.


## Local performance investigation

The initial implementation at `e52af023fc` completed all 145 screening points
from immutable source/binary snapshots. Its mcf/6753 directed test
(2M warmup, 4M total instructions, same pinned dedup REF) passes difftest but
shows a regression: IPC LRU 0.776126, XSDRRIP 0.773607, ICP 0.733915. ICP uses
low-priority insertion for 101,342 of 123,039 prefetch fills, with 1,027 EPF
corrections and 84,346 EPF displacements. These are one-point diagnostic results,
not full-suite performance conclusions.

Source inspection found an extra negative-training event when a new prefetch
superseded an old EPF record. The paper trains a failure on EPF displacement,
not on re-prefetch alone. A directed sequence reproduces the issue: re-prefetch
an evicted low-confidence line, consume it, then issue another prefetch. The
initial implementation incorrectly leaves the last prefetch at low priority.
The correction retires superseded evidence neutrally and exposes
`epfSuperseded`. The directed sequence fails before this correction. The
six behavioral checks pass after the correction; the new sequence fails with
both the original opt and original Clang fast binaries. Full-suite results
below distinguish the initial implementation from subsequent corrections.


### Optional PC-granularity accuracy predictor

`--icp-pc-entries=64` gives each prefetch source 64 hashed PC counters plus one
source-only fallback counter for requests without a PC. It approximates the
paper's per-stream predictor using metadata already attached to most prefetch
requests. The table defaults to zero PC entries, preserving the source-only
experiment until performance evidence supports changing that choice.

Counters remain 4-bit and all sources remain separate. Each resident prefetch
and EPF record retains its originating predictor index, so later demand use or
expiration trains the same bucket. The table has bounded storage:
`NUM_PF_SOURCES * (pc_entries + 1) * counter_bits` counter bits per L2 slice,
plus predictor indices on tag/EPF metadata. Hash collisions can merge PCs;
PC is an available proxy for a stream rather than an exact stream identifier.
There are no prefetch-generation changes. Predictor access and training cost
O(1); existing stack and EPF bounds remain unchanged.

`pcPrefetchFills` and `fallbackPrefetchFills` expose PC coverage, while
`lowFillsBySource` identifies which sources receive low-priority insertion.
TraceGen now preserves the packet format's existing optional PC field, enabling
a two-stream hardware-prefetch test. The test proves that a success on one PC
raises only its own confidence when PC buckets are enabled. All seven scenarios
pass with both zero and 64 PC entries (14 checks). GCC opt also builds and passes PC-isolation and superseded-record checks in
both modes (four additional checks).

Directed checkpoint A/B (2M warmup / 4M total instructions, Clang fast) passes
difftest for all candidates. On GemsFDTD/65615, PC buckets improve IPC from
5.724234 (source only) to 6.132436, while LRU is 6.306927 and XSDRRIP is
6.362302. On mcf/6753, source-only IPC is 0.741689 and PC buckets produce
0.734951; PC buckets with 1024 EPF entries reach 0.741911. All measured
prefetch fills in these two PC-enabled tests carry a PC. Finer granularity
helps GemsFDTD but does not resolve mcf, so this remains an optional candidate;
no default or full-suite speedup claim follows from these directed results.


### Conservative low-confidence retention

`--icp-low-priority` controls the rank used for predicted-inaccurate prefetches.
The default `-1` retains the paper's LRU position. An explicit rank gives an
uncertain prefetch a bounded opportunity to survive more intervening fills;
high-confidence insertion remains independently controlled. Ranks must satisfy
`0 <= high <= low < associativity`. This adds no predictor or queue storage and
retains the same bounded stack operations.

The `low-retention` packet test proves the tradeoff directly: an LRU-inserted
prefetch is evicted by the next demand miss, whereas rank 2 in a four-way cache
survives and supplies the subsequent demand hit. Demand misses change from 6 to
5. All eight scenarios pass in both source-only and 64-PC modes, plus both
explicit retention tests (18 fast checks). Four focused GCC opt checks also
pass, covering both PC modes at default LRU and rank 2 insertion.

Directed mcf/6753 results (same 2M/4M window) improve to IPC 0.769963 with
64 PC buckets and low rank 4, compared with 0.734951 for PC buckets with LRU
insertion. This is still 0.79% below LRU. GemsFDTD/65615 reaches 6.299649 with
the same configuration (0.12% below LRU); source-only rank 4 reaches 6.327700,
but performs worse on mcf. Low ranks 2 and 6 were also evaluated. A single
balanced candidate, 64 PC buckets and low rank 4, is selected for complete
145-point screening. This selection does not establish an aggregate gain.

### Matched reference screening

Both existing reference runs completed successfully at source
`db4900752a8dca7245601b74350a4cce15e9cc49`. Their LRU/XSDRRIP implementations
are unchanged from this branch's base. All 145 points completed, their archived
checkpoint lists and weights match the local screening inputs exactly, and
the effective configurations match after excluding the replacement policy and
the location of the identical DRAM INI. The workflow builds DRAMsim3 from the
same shared source cache used locally.

| Reference | Actions run | Score/GHz |
| --- | --- | ---: |
| LRU | 43269172688 | 24.25147898539802 |
| XSDRRIP | 43269179825 | 24.17447693774795 |

Scores were independently regenerated using the shared `gem5_data_proc`
`run.py` pipeline with the archived `cluster.json`. Each raw CSV contains all
145 points; the derived weighted and score CSVs aggregate benchmarks and
reproduce the archived total scores. Legacy wrapper-incompatible L2 aliases
can report zero, so L2 demand analysis uses the `l2_demand` YAML group and raw
slice counters instead. The normal analysis environment lacks SciPy; a separate
temporary SciPy installation was used without modifying the shared tool or
user environment. ICP full-profile confirmation and its remote run remain pending.


### Additional validation and rejected hit-promotion variant

The GCC `gcc_cpdecl/1944` 2M/4M diagnostic also passes difftest for LRU,
XSDRRIP, source-only/PC64 with LRU insertion, and source-only/PC64 with low
rank 4. IPC is 1.727399 for LRU, 1.701265 for XSDRRIP, 1.726555 for source-only
rank 4, and 1.718496 for PC64 rank 4. This short window does not reproduce
exactly the regression magnitude of the full 20M/40M screening window.

A temporary variant promoted every prefetch hit to MRU regardless of its
accuracy. Nine diagnostic variants and one default control completed with
difftest. The GCC default control reproduces the original candidate's ticks,
measured instructions, IPC, and L2 demand misses exactly. With PC64 and low
rank 4 held constant, the hit-promotion change produces:

| Diagnostic point | Selective hit promotion IPC | Unconditional promotion IPC |
| --- | ---: | ---: |
| mcf/6753 | 0.769963 | 0.760130 |
| GemsFDTD/65615 | 6.299649 | 6.299649 |
| gcc_cpdecl/1944 | 1.718496 | 1.711545 |

All entries use the same 2M warmup/4M total window. The variant does not give
a consistent gain, so its configuration and implementation were discarded.
The branch retains the existing accuracy-based prefetch-hit handling.

The completed GemsFDTD/43464 candidate point uses the full 20M/40M window.
Its normalized speed relative to the matched LRU reference changes from
-7.496% for the initial implementation to -0.241% for PC64/low rank 4. This
is a point result; complete 145-point results are reported below.

### Full-profile reference audit

The existing 1.0c references also completed successfully at source
`a61859f355cf013aa6c15f95b6be0fe24b829976`:

| Reference | Actions run | Points | Score/GHz |
| --- | --- | ---: | ---: |
| LRU | 43271285986 | 1094 | 22.96817155872683 |
| XSDRRIP | 43271296003 | 1094 | 22.916332459687805 |

Both archived checkpoint lists and weights match the full local profile
byte for byte. Original LRU/XSDRRIP header and implementation blob IDs match
this branch exactly. Effective sample configurations match the screening
references apart from the identical DRAM INI's location, and both runs use
20M warmup/40M total instructions through the configuration defaults.

The shared scoring pipeline independently reproduces both scores. Each raw
CSV contains exactly the 1094 point names in the weights JSON, and score CSVs
contain 29 benchmark rows plus integer, floating-point, and overall summaries.
Summary rows must be excluded from a second geometric-mean calculation.
The minimum reported coverage, 0.999743155 for cactusADM, equals that workload's
weight sum in the original profile; all listed points are present. An ICP
full-profile result and remote candidate run are still required.


### Further full-window retention controls

The `gcc_200/5108` regression persists at 20M warmup/40M total instructions.
The following controls use that same window, pinned REF, difftest, and cache
geometry. Positive values indicate normalized speed above the original LRU.

| Configuration | IPC | Speed vs LRU |
| --- | ---: | ---: |
| pc64-low1 | 2.206715 | +0.3576% |
| Always MRU: threshold 0, low rank 0 | 2.198852 | +0.0000% |
| pc64-low2 | 2.146812 | -2.3667% |
| pc1024-low4 | 2.132456 | -3.0196% |
| pc256-low4 | 2.121609 | -3.5129% |
| pc16-low4 | 2.120124 | -3.5804% |
| pc0-low4 | 2.106503 | -4.1999% |
| pc64-low0 | 2.095390 | -4.7053% |
| PC64, low rank 4 (screening candidate) | 2.112488 | -3.9277% |

At threshold 8, low-confidence prefetch hits retain their previous rank.
Threshold 0 also promotes these hits. This always-MRU control matches the
original LRU ticks, instructions, and IPC exactly in both warmup and final
measurement. The warmup cycle and L2 demand-miss counts also match.

PC64 with low rank 1 is selected for additional full-window mcf and GemsFDTD
checks. The completed GemsFDTD/65615 result is IPC 5.509482, +0.4978% over
LRU and +0.3201% over XSDRRIP. An aggregate gain is still unproven.

### Experimental feedback from prefetch reads

The cache clears its prefetched marker when the first read supplies data,
including a read issued by an upper prefetcher. Demand-only ICP feedback can
leave its separate unused-prefetch marker set until the line is evicted.
A prototype `--icp-credit-prefetch-hits` experiment credited a resident
prefetch once on a prefetch read tag hit. The originating source/PC counter
received the update. A later demand use received no second credit. The
existing accuracy rule determined hit promotion after training; the first
credited use also honored the optional demotion setting.

The replacement callback observes tag matches before permission checks.
This event is a proxy for application usefulness. A speculative
prefetch read may still never benefit a CPU demand, so complete workload
comparisons are needed to judge its value. The prototype retained EPF
demand-miss correction. It defaulted to disabled and has been removed from
the retained branch after the full-window controls below.

The initial prototype passed 30 Clang fast packet checks and 12 focused GCC
opt checks. These
cover single-use accounting, the originating PC, and demotion interaction.
The new/old default control on gcc_200/5108 reproduces ticks, instructions,
IPC, prefetch fill/use counts, and L2 demand misses exactly.

The following diagnostic IPCs all use 2M warmup/4M total instructions:

| Configuration | mcf/6753 | GemsFDTD/65615 | gcc_200/5108 |
| --- | ---: | ---: | ---: |
| Credit, source only, low LRU | 0.735247 | 6.311777 | 2.855831 |
| Credit, PC64, low LRU | 0.738422 | 6.330951 | 2.855831 |
| Credit, PC64, low rank 4 | 0.772400 | 6.306927 | 2.855831 |
| Original LRU | 0.776126 | 6.306927 | 2.855831 |

All 13 diagnostic variants and controls completed with difftest. The short
GCC window is insensitive to these changes. Full 20M/40M results for mcf,
GemsFDTD, and GCC are reported below; GCC compares low ranks 1 and 4.
Observed prefetch-hit requests carry PFDepth value 0 in these tests;
the feedback rule does not require a depth classification.


### Completed PC64 rank 4 screening

All 145 points completed without aborts. The scorer uses the matched 0.3c
weights and validates final 20M instruction blocks after 20M warmup.

| Policy | Execution | Score/GHz |
| --- | --- | ---: |
| LRU | Actions run 43269172688 | 24.251478985 |
| XSDRRIP | Actions run 43269179825 | 24.174476938 |
| ICP, initial `e52af023fc` | Local frozen snapshot | 23.953828062 |
| ICP, PC64, low rank 4 | Local frozen snapshot | 24.236283867 |

The candidate is -0.0627% versus LRU and
+0.2557% versus XSDRRIP. It improves on
XSDRRIP but has not achieved an overall improvement against both references.

| Benchmark | Speed vs LRU |
| --- | ---: |
| zeusmp | -0.6915% |
| omnetpp | -0.5169% |
| cactusADM | -0.4675% |
| gcc | -0.4502% |
| mcf | -0.3700% |
| gobmk | +0.2956% |
| bwaves | +0.2414% |
| xalancbmk | +0.1865% |

The additional PC64 rank 1 mcf/6753 result is IPC 0.743522, -0.3401% versus LRU and
+0.7792% versus XSDRRIP. Together with the GCC and
GemsFDTD full-window controls, it supports testing rank 1 on all 145 points.
That new screening uses an independent copy of the same frozen runtime,
PC64, low rank 1, and the existing feedback rule. A complete comparison
against both references is required before selecting it.


### Completed feedback controls

The following results use 20M warmup/40M total instructions with PC64.
All eight feedback variants completed with difftest. Positive values indicate
normalized speed above the matched LRU point:

| Point | Low rank | Prefetch-read credit | Credit plus first-use demotion |
| --- | ---: | ---: | ---: |
| gcc_200/5108 | 1 | -1.7983% | -3.0610% |
| gcc_200/5108 | 4 | -3.6960% | -3.3851% |
| GemsFDTD/65615 | 4 | +0.6410% | -0.1079% |
| mcf/6753 | 4 | -0.7528% | -3.3221% |

The demotion combination does not give a consistent gain. A further GCC
control with corrected hit behavior, high rank 1, low rank 4, PC64, and
credit enabled also completes with difftest: IPC 2.122438, -3.4752% versus
LRU and -3.0579% versus XSDRRIP. Moving accurate-prefetch insertion below
MRU does not resolve the regression. The credit flag, its extra statistics,
and its packet scenarios are therefore removed from the retained branch.
The frozen prototype and results remain available locally for reproduction:
`/tmp/icp-recency-fast` and `/tmp/icp-prefetch-use-high1-full`. The removed
patch is saved as `/tmp/icp-rejected-prefetch-read-credit.patch`.

### Prefetch hit recency correction

The nonzero `high_priority` setting previously also controlled the position
assigned on an accurate prefetch hit. With high rank 1, a prefetch hit could
move a currently MRU line down to rank 1. The setting now controls insertion;
accurate prefetch hits promote to MRU. The existing rank-1 full screening
uses high rank 0, where the old and corrected hit updates are identical.

The `prefetch-recency` cache-packet test first demand-touches a line, then
issues an accurate software-prefetch hit and three conflicting demand fills.
The old frozen binary reports 8 demand misses instead of the expected 7 at
high rank 1 in both source-only and PC64 modes. Both high-rank-0 controls pass.
The corrected implementation passes all 34 Clang fast checks and 16 focused
GCC opt checks. Earlier high-rank-2 experiments used the previous hit rule;
their results describe that historical implementation.

The archived prototype uses raw prefetch-hit depth names `depth0` through
`depth3`, plus `other`, matching the request PFDepth value directly. All
observed credited hits have value 0; no hierarchy level is inferred from it.
These diagnostic counters were removed together with the prototype.

A boundary audit checks all 145 points from the initial implementation, the
PC64 rank-4 candidate, and each reference. All 580 results have separate
warmup and measurement blocks near 20M instructions, increasing final ticks,
and the expected instruction-limit exit. Completed points in the ongoing
rank-1 screening pass the same audit; its total score still awaits coverage.


### Requestor-aware regression diagnosis

The L2 `demandMisses::total` vector includes requests from upper-level
prefetchers in this hierarchy. It must not be presented as a count of CPU
demand misses alone. Requestor suffixes such as `cpu.data`,
`cpu.mmu.dtb.walker`, and `cpu.dcache.prefetcher` distinguish these paths.
The scoring result remains normalized execution time; these counters help
explain behavior but do not substitute for that result.

For `gcc_cpdecl/1944`, all entries below use the full 20M/40M window:

| Policy | Speed vs LRU | CPU data L2 misses | Upper-prefetcher L2 misses | HardPFReq MSHR misses |
| --- | ---: | ---: | ---: | ---: |
| LRU | 0.0000% | 30,566 | 108,536 | 166,684 |
| ICP PC64, low rank 4 | -0.3676% | 27,569 | 99,060 | 136,313 |
| ICP PC64, low rank 1 | -1.6703% | 30,001 | 107,405 | 163,408 |

Upper-prefetcher misses sum the `cpu.dcache.prefetcher` and
`cpu.dcache.prefetcher.lldp` requestors. Hardware-prefetch MSHR misses use
the separate `HardPFReq.mshrMisses::total` statistic. CPU data misses
decrease for both ICP settings, while measured execution slows. This
does not identify a unique cause; prefetch timing and the critical
miss sequence remain unresolved factors. Rank 1 also inserts 268,821
prefetches compared with 233,567 for rank 4.


### Validation of the retained implementation

After retiring the prefetch-read-credit prototype, GCC opt and Clang 19
fast both rebuild successfully. Each passes the same 22 cache-packet
checks: demand and EPF feedback, secure separation, first-use demotion,
merged demand, superseded records, PC isolation, low-rank retention, and
the nonzero-high-rank recency regression. Repository style checks for the
retained policy, parameters, and test script pass, as does the whitespace
check. The retained runtime is committed at `6f1545f6c6`.

Local build/check manifests are in `/tmp/icp-retained-build-results.json`.
The older 34/16 check counts above describe the archived credit prototype;
its removed scenarios do not belong to the retained test set.


### Completed PC64 rank 1 screening and additional controls

All 145 rank-1 points completed without aborts. A full audit checks every
point's instruction-limit exit, separate approximately 20M warmup and 20M
measurement windows, all four L2 slice policy configurations, and instruction
boundaries against both references. The runtime differs from the retained
source only in the equivalent high-rank-0 hit update and parameter wording.

| Policy | Score/GHz | Speed vs LRU | Speed vs XSDRRIP |
| --- | ---: | ---: | ---: |
| ICP PC64, low rank 1 | 24.236022160 | -0.0637% | +0.2546% |
| ICP PC64, low rank 4 | 24.236283867 | -0.0627% | +0.2557% |

Rank-1 benchmark regressions versus LRU include zeusmp (-0.8141%), omnetpp
(-0.6133%), cactusADM (-0.5326%), leslie3d (-0.3604%), and mcf (-0.2435%).
The similar aggregate scores conceal differences between individual workloads.

Moving accurate insertion from MRU to rank 1 with low rank 1 and demand-only
feedback also fails to resolve the regressions: cactusADM/163 is -0.5370%
versus LRU, while gcc_200/5108 is -2.6282%. These runs use the corrected
prefetch-hit MRU behavior and full 20M/40M windows.

A separate prototype promoted prefetch hits on demand-fetched or already
demand-used lines irrespective of incoming confidence, while continuing to
suppress hits on unused inaccurate prefetches. It passed 28 packet checks,
including explicit no-double-credit and suppression controls. Full-window
results show why it was removed:

| Point | Low rank | Speed vs LRU | Speed vs same-rank retained ICP |
| --- | ---: | ---: | ---: |
| gcc_200/5108 | 1 | -4.1561% | -4.4976% |
| gcc_200/5108 | 4 | -3.4438% | +0.5037% |
| gcc_cpdecl/1944 | 1 | -0.8697% | +0.8142% |
| gcc_cpdecl/1944 | 4 | -0.3674% | +0.0003% |

A packet-level recency improvement therefore does not establish a workload
performance benefit. The prototype's exact patch and summarized results are
preserved under `~/.tmp/icp-l2-20261114/retired/`. The full rank-1 audit is
`~/.tmp/icp-l2-20261114/audit/rank1/audit.json`.

### Experimental sampled LRU/ICP selection

Static rank changes and hit-promotion changes trade performance between
workloads without an aggregate gain over LRU. An optional `--icp-adaptive`
experiment therefore compares LRU recency and the configured ICP policy in
fixed sampling sets. It uses no workload identifiers or offline scores.

With the normal 1024-set slice and sample stride 32, an invertible permutation
of set-index bits assigns 32 LRU leaders and 32 ICP leaders. XOR shifts, an
odd multiplicative constant, and the set mask spread the leader groups across
regular address strides while preserving their exact counts. Adaptive mode
requires a power-of-two set count and sample stride. All other sets follow a
slice-local 10-bit saturating selector. LRU leader demand fills
increment the selector toward ICP; ICP leader demand fills decrement it
toward LRU. Values at least 512 select ICP; the initial value 511 selects LRU.
Leaders retain their assigned policies independently of the selector.

Training observes non-prefetch read-response allocations and direct demand
request allocations. A demand merged into a prefetch fill trains once through
the existing fill-completion callback. Prefetch-only fills and hits do not
train the selector. This is an allocation-based demand-miss proxy; it does
not count every permission miss or MSHR-merged request separately.

The LRU alternative inserts and touches at MRU and suppresses first-use
demotion. Accuracy evidence remains active in both modes, but only actual
ICP low-priority insertions enter EPF on unused eviction. Policy changes
apply to subsequent accesses and insertions; they do not reorder all resident
lines. Followers expose separate LRU/ICP prefetch-fill counts, while leader
fill counts, switch counts, and a live selector value explain the choice.
The live value remains valid after statistics are reset at warmup.

The experiment defaults to disabled, preserves cache geometry and modeled
latencies, and adds constant-time selection/training to the existing bounded
rank/EPF work. The simulator caches set roles in a bounded array. Sampling
bias and training lag may reduce performance; the leaders themselves also
retain the less successful policy in part of the cache. This extension is
an optimization experiment rather than part of the original paper's policy.

Clang fast and GCC opt each pass 36 packet checks: the 22 retained scenarios
plus 14 sampled policy checks across source-only and PC64 predictors. The new
cases verify leader behavior, follower switches in both directions, saturation
at both limits, no prefetch-only training, one-time credit for demand-merged
prefetch fills, and exactly 32 leaders per policy when all 1024 set-index bits
are exercised. No suite gain is inferred from these functional tests.

The initial sampler used the XOR of the low and next five set-index bits.
Its frozen implementation is `~/.tmp/icp-l2-20261114/adaptive-fast` and passed
34 checks with each compiler. All four full-window scouts completed:
+0.5812% versus LRU on GemsFDTD/65615, -1.2291% on zeusmp/66319,
-0.5995% on gcc_200/5108, and -1.1497% on mcf/6753. The zeusmp sampling leaders favor ICP even though retained
whole-slice CPU data miss counts and cumulative miss latency exceed LRU.
This motivates testing the permuted sampling cohorts separately; it is a
hypothesis about representativeness, not proof of a cause.

The permuted sampler is frozen independently at
`~/.tmp/icp-l2-20261114/adaptive-mixed-fast`. It compares the same four full
20M/40M workload windows with the original cohorts and matched LRU/XSDRRIP
references. Completed points so far are +0.9285% versus LRU on
GemsFDTD/65615, -0.9084% on zeusmp/66319, -3.1155% on gcc_200/5108,
and -0.5999% on mcf/6753; all four scouts are complete.
The new sampling distribution improves the first two points but worsens GCC;
no uniform benefit follows from the sampling change. Separate rank-1 scouts
use the same source to test whether shallower insertion limits that regression.
All four rank-1 points are complete: +0.7126% versus LRU on
GemsFDTD/65615, -2.4828% on gcc_200/5108, -2.3551% on zeusmp/66319,
and -0.2123% on mcf/6753. This improves GCC relative to rank 4 but worsens zeusmp.
No aggregate score follows from scouts.

The GCC control with adaptation disabled exactly reproduces retained static
ICP ticks, instructions, and IPC in both warmup and final measurement.
The full 145-point permuted/rank-4 screening runs with 16 workers in parallel
with the remaining slow scouts. Its scorer requires that exact control plus
complete point, configuration, and measurement-window audits before producing
an aggregate result. Candidate selection and suite performance remain pending.

Two additional always-MRU controls set high/low rank and accuracy threshold
to zero with adaptation disabled. They compare GemsFDTD/65615 and
zeusmp/66319 directly against original LRU to distinguish insertion effects
from rank/timestamp tie handling or training-only request treatment. The
prior always-MRU GCC control already matches original LRU exactly. Both new
controls also pass: warmup and measurement ticks, instructions, and IPC match
original LRU exactly, yielding 0.0% speed difference. Their result manifests
are under `experiments/always-mru-controls` in the temporary experiment root.
This rules out a measurable rank-versus-timestamp difference for these windows;
it does not show that sampled selection is beneficial.

A further parameter-only diagnostic holds the frozen binary, PC64, and low
rank 4 fixed while changing sample stride from 32 to 64. All four effective
configurations are checked: each 1024-set slice has 16 leaders per policy.
This tests whether fewer fixed experimental sets reduce interference, at the
cost of slower selector training. The four full-window scouts remain pending.
An initial override used printed object names instead of Python vector indices
and failed before simulation; those logs are preserved separately and excluded.

GCC's permuted/rank-4 warmup uses 9,110 ICP follower prefetch fills in slice 0
and 7,940 in slice 3, despite ending warmup at selector 511 in both. In the
measured window, the total follower counts are 49,353 LRU and 790 ICP, yet
L2 hardware-prefetch MSHR misses total only 5,509 versus 34,601 for original
LRU. The shared L2 prefetcher issues 12,577 requests versus 209,613 for LRU;
CPU-data L2 misses are 11,153 versus 10,245. These observations are consistent
with persistent cache/prefetcher state from earlier decisions. They do not
isolate historical effects from the fixed leader sets or prove a unique cause.
The raw phase comparisons are saved under
`~/.tmp/icp-l2-20261114/audit/adaptive-scouts/comparison.json`.

An independent incremental auditor additionally checks both reference windows
and the actual instruction boundaries for each completed screening point. It
also requires both warmup and final scout statistics to match exactly wherever
a scout has a corresponding completed suite point. Partial coverage is reported
only as audited point counts, never as a suite score.

Full 1.0c confirmation still covers all 1094 points. Its checkpoint list and
weights have been verified and its matched reference score CSVs copied under
`~/.tmp/icp-l2-20261114/full-reference-scores`. A separate full-profile launcher
and scorer are prepared; screening alone does not establish final success.

### Correct demand feedback for upper-level prefetch merges

A request from the CPU side of a cache is not necessarily a CPU demand.
Upper-level hardware prefetches reach L2 as ReadReq carrying Request::PREFETCH.
The original fill callback used any FromCPU target as demand evidence, and
required a local FromPrefetcher target before invoking the replacement policy.
This produces two symmetric errors: an upper-level prefetch merged into a local
prefetch trains a false demand use, while a real demand merged into an
upper-level prefetch receives no completion credit.

Eight packet regressions reproduce these errors in the frozen sampler binary:
both leader roles and both source-only/PC64 modes fail their expected selector
training count. The correction collects true demand targets independently of
upstream-side targets and invokes the replacement-policy completion callback
for every valid fill. ICP itself checks whether that residency was prefetched.
The existing cache prefetch marking and prefetcher feedback paths retain their
own upstream/local classification; this change concerns replacement feedback.
Both pure-prefetch merges and real demand merges now receive the intended
training, with a subsequent demand hit checking that merged use is credited
only once.

Clang fast and GCC opt each pass all 44 packet checks, including the existing
36 scenarios and eight new upper-level merge checks. Scoped repository style
and whitespace checks pass. The fixed runtime is frozen independently at
`~/.tmp/icp-l2-20261114/adaptive-mergefix-fast`; earlier sampler results above
use the previous completion callback and are retained as historical evidence.
Four static and four adaptive full-window scouts plus an exact original-LRU
GCC always-MRU control are running with the corrected source. Their results
must precede any performance conclusion about the corrected implementation.

The corrected full-window scouts confirm an exact original-LRU match for the
GCC always-MRU control in both warmup and measurement. Corrected static/rank-4
ICP is -0.7216% versus LRU on gcc_200/5108, +0.8933% on GemsFDTD/65615, and
-1.7979% on zeusmp/66319. Corrected adaptive/rank-4 ICP is -3.7422%, +0.3408%,
and -0.7192%, respectively; both mcf scouts remain in progress. Independent
145-point static and adaptive screens now run from immutable source
`8d8c98fedd` with 32 local workers each. These are separate experiments with
matched point lists, weights, REF, and 20M/40M windows.

### Independent prefetch-hit promotion control

The author policy defaults use-accuracy-prefetch-hit to false, independently
of its insertion-accuracy predictor. Exposing that choice lets this experiment
test accuracy-based insertion without suppressing recency updates on hits.
The optional `--icp-promote-all-prefetch-hits` flag sets
`ICPRP.accuracy_on_prefetch_hit=False`; the parameter defaults to true, retaining
the prior experiment when the flag is omitted. All prefetch hits then promote
the resident line to MRU. Confidence lookup, insertion rank, demand-use credit,
and EPF training retain their existing behavior.

More promotions may protect useful lines or retain pollution longer. A directed
four-way sequence demonstrates the behavioral distinction: an inaccurate
prefetch enters at LRU, a second prefetch hits it, and a demand fill intervenes.
With hit filtering, that fill evicts the prefetched line; with unconditional
promotion it survives and satisfies the next demand. The sequence checks the
insertion class, useful/unused accounting, EPF correction, ignored-hit count,
and actual demand misses. Clang fast and GCC opt each pass all 48 cache-packet
checks, including both modes and source-only/PC64 predictor configurations.
Scoped repository style and whitespace checks pass.

The new immutable snapshot is `~/.tmp/icp-l2-20261114/insertion-only-fast`.
Four full-window insertion-only scouts use PC64, high rank 0, low rank 4,
adaptation disabled, and unconditional prefetch-hit promotion. A fifth GCC
control leaves the new flag unset and must exactly reproduce the corrected
static ICP warmup and measurement metrics. These experiments remain in progress;
no suite improvement has yet been demonstrated.
