# FDP L2 dynamic insertion study

## Sources and scope

`docs/CRP.pdf` is *Cache Replacement Policies* (Akanksha Jain and Calvin Lin,
2019). Section 5.4.1 (printed page 49) cites:

Santhosh Srinath, Onur Mutlu, Hyesoon Kim and Yale N. Patt,
*Feedback Directed Prefetching: Improving the Performance and
Bandwidth-Efficiency of Hardware Prefetchers*, HPCA 2007, pp. 63–74.
DOI: 10.1109/HPCA.2007.346185.

The author-hosted paper was downloaded to
`docs/papers/srinath-hpca2007-feedback-directed-prefetching.pdf` from
https://users.ece.cmu.edu/~omutlu/pub/srinath_hpca07.pdf.
The PDF is a local reference and is not included in the public code commit.

CRP summarizes FDP as accuracy-based insertion. The original paper's
Section 3.3.2 specifically selects insertion positions using **pollution**;
accuracy and lateness also control prefetch aggressiveness in the full FDP.
This implementation includes the replacement/insertion part and its pollution
feedback. It does not change prefetch generation, degree, or distance.

Public source search (GitHub, 2026-11-13) found independent implementations:

- https://github.com/pchunduri6/Feedback-Directed-Prefetching
  (`prefetcher/feedback_prefetcher.c`, DPC2 simulator).
- https://github.com/Satjpatel/Implementation-and-Comparison-of-GHB-Based-Stride-Prefetcher-Feedback-Directed-Prefetching-and-a-C
  (coursework in a ChampSim fork).

The DPC2 example contains pollution tracking and stream-aggressiveness control,
but exposes no adaptive insertion implementation. Neither repository is
established as the original authors' simulator/code.
The gem5 implementation is written from the paper; no third-party code is copied.

A later search found the author-linked **CMU-SAFARI/memsim** simulator:
https://github.com/CMU-SAFARI/memsim at
`45e57250c9b60f29243c9c826ec7d9bf9bd75051`.
Onur Mutlu's https://users.ece.cmu.edu/~omutlu/projects.htm links it as the
source of the 2015 informed-caching work. Its `Simulator/CmpFDP.h` implements
a pollution-based FDP baseline; `Simulator/CmpFDPAP.h` implements an
accuracy-based variant. The former uses a tagged evicted-address filter and
two insertion priorities with a 25% threshold, rather than the 2007 paper's
bit filter and three positions. This is released source containing FDP
baselines, not evidence of the exact original 2007 simulator's release.
A source snapshot with its README/license is saved locally as
`docs/papers/memsim-fdp-reference-45e57250.tar.gz`.

## Model

- Exact LRU stack within each L2 set, with MRU demand insertion and LRU hit
  promotion. An eight-way stack inserts prefetches at zero-based MRU ranks
  4 (MID), 6 (LRU-4), or 7 (LRU).
- A 4096-bit XOR-indexed pollution filter records demand-filled blocks evicted
  by prefetch fills. A prefetch refill clears its bit. Demand tag misses consult
  the filter. Secure and non-secure domains have separate bit vectors.
- Only committed capacity evictions train the feedback. Failed allocation
  attempts and coherence invalidations do not count as capacity evictions.
- Useful evictions (demand-filled or consumed prefetch blocks) clock intervals.
  The default interval is half the lines of each L2 slice, preserving the paper's
  interval/capacity ratio. Each L2 slice has independent feedback.
- At interval boundaries, demand and pollution counters use the paper's
  `new = (old + interval) / 2` smoothing. Pollution below 0.005 selects MID;
  above 0.25 selects LRU; other values select LRU-4. Startup uses MID and an
  interval without demand evidence retains the previous choice.
- Existing LRU/XSDRRIP policies receive default empty observation hooks.
  Their replacement decisions are unchanged.

## Configuration and validation

Both `kmhv3.py` and `idealkmhv3.py` accept
`--l2-replacement-policy={default,lru,xs-drrip,fdp}`. The default preserves the
original configuration. Explicit policies apply to classic L2 and all aligned
L2 slices. Cache size, ways, and line size determine policy geometry.
FDP parameters can be overridden through the existing `--param` interface.

Unit tests cover exact insertion ranks and promotion, partial/invalid sets,
randomized stack-rank invariants, pollution attribution and hash behavior,
security domains, useful-eviction interval clocks, smoothing, and all insertion
positions. A full simulator build and checkpoint smoke run check integration.

Remote comparisons use the same exact source commit, configuration, checkpoint
set, warmup, prefetcher, and memory model; only the L2 policy changes. Full-suite
coverage, benchmark regressions and cache/feedback counters must be inspected
before claiming a performance improvement.

## Results

Local checks so far: seven core GoogleTest cases passed; repository style
checks passed for new code and modified regions; Python syntax and generated
SimObject configuration construction passed (8-way, 1024-set L2 slice and
4096 useful evictions per interval). The optimized simulator build passed. Checkpoint smoke validation and remote
measurements are in progress. The mutable legacy REF path failed identically
for all three policies in the checkpoint restorer; smoke tests therefore use
the verified `util/nemu_ref/resolve.py normal` release, as the workflow does
for PGO (the workflow uses its normal-dedup variant for benchmark runs).


Initial remote screening (all runs use source `db4900752a`):

| Policy | Run ID | Configuration | Coverage |
| --- | --- | --- | --- |
| LRU | 43269172688 | idealkmhv3.py | gcc16 SPEC06 0.3c, all 145 slices |
| XSDRRIP | 43269179825 | idealkmhv3.py | gcc16 SPEC06 0.3c, all 145 slices |
| FDP paper | 43269188685 | idealkmhv3.py | gcc16 SPEC06 0.3c, all 145 slices |

All three explicitly use 20M warmup instructions and 40M total instructions,
the default distributed server pool and 16 jobs/server. These are screening
measurements; final performance claims require full-coverage confirmation.


Analysis tooling: the personal default `gem5_data_proc/run.py` exists but is
not readable by this account. Use the workflow's accessible shared copy at
`/nfs/home/share/gem5_ci/gem5_data_proc/run.py`, with the archived `cluster.json`
passed explicitly. No additional tools were installed.


### Tagged pollution-feedback candidate

The mcf/6753 integration smoke (2M warmup + 2M measured instructions) passed
NEMU difftest for all three policies. Measured IPC: LRU 0.776126,
XSDRRIP 0.773607, paper FDP 0.745430. This is a short local reproduction,
not the weighted suite result. Paper FDP selected LRU insertion for
121018 / 123060 prefetch fills during measurement, with 15398 pollution-filter
hits in 57158 eligible demand misses.

The optional `FDPRP.tagged_filter=True` candidate retains the same
MID/LRU-4/LRU rule and thresholds, but validates eviction records with block
addresses, consumes a record after one demand miss, and retires it upon a
new residency. This tests whether stale or aliased bit-filter evidence causes
excessive low-priority insertion. `filterAliasesRejected` makes rejected aliases
observable. The default remains the paper's bit-filter policy until performance
validation supports a change. The tagged variant adds address-tag storage;
it is an algorithm modification, not a claim of identical hardware cost.

Nine core unit tests and the optimized build pass with both filter modes.
The tagged mcf smoke and weighted remote comparisons are in progress.

For final full-coverage comparison, the independent AMPM session has already
launched compatible baseline candidates at source
`a61859f355cf013aa6c15f95b6be0fe24b829976`:
LRU `43271285986`, XSDRRIP `43271296003`. Source inspection confirms the same
L2 geometry and default 20M warmup/40M total. Archived effective configs,
checkpoint weights, and REF must still be compared before these are reused.


### Per-source protection experiment

`FDPRP.source_feedback=True` adds independent 6-bit saturating usefulness
confidence per prefetch source. Counters start at 32, increase on first demand
use, decrease on unused capacity eviction, and protect sources at MRU when
confidence reaches 48. This is confidence, not a calibrated accuracy percentage.
Unknown sources retain ordinary FDP insertion. The default remains disabled.

Eleven unit cases and the optimized build pass. The tagged+source mcf smoke
passes NEMU difftest, but its IPC 0.763816 is 0.29% below tagged-only 0.766017.
It protects 12891 DespacitoStream fills in the measurement period. It therefore
has not demonstrated a mcf improvement and is not selected as the default.
Its usefulness for other regressing benchmarks remains an experiment.

The existing classic-L2 configs referenced the removed `slice_num` parameter;
they now use the current `num_slices`. Short 100K-instruction NEMU-difftest
runs pass for ideal/classic, kmhv3/classic, and kmhv3/sliced L2. This correction
does not affect the aligned L2 configuration used for remote comparisons.

The full LRU and XSDRRIP baseline archives have now been checked against their
matching screening configs. Every effective setting matches; the only text
difference is each runner's DRAM configuration path, whose contents have the
same SHA-256. Their archived weights match the full 1094-point JSON. The
machine-readable equivalence evidence is in `docs/experiments/fdp-l2/`.


### Merged-demand use accounting

The cache services MSHR demand targets directly from fill data, without a
second `BaseSetAssoc::accessBlock` call. FDP now receives a separate demand-use
notification on this path. A prefetched entry is counted as used once,
promoted according to demand-use LRU semantics, and (when enabled) positively
trains its source confidence. Subsequent consumers cannot count it twice.
Legacy replacement policies have an empty hook, preserving their decisions.
Earlier v1–v3 experiments predate this accounting correction; final performance
claims must use a candidate containing the fix. Eleven core tests and the
optimized build pass; checkpoint regression measurements are pending.
