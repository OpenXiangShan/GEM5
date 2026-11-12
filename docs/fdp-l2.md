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
4096 useful evictions per interval). Full build and checkpoint smoke validation
are in progress. Remote measurements are pending.
