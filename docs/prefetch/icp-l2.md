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
is counted at most once per residency. EPF entries include full physical line
addresses and the secure bit; unlike the paper's partial tags this has no tag
alias false positives. The current composite prefetchers expose source IDs but
not a uniform stream ID, so source-specific counters replace paper stream IDs.

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

Experimental branch: `icp-l2`, based on `f9649a04ee`.
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
build/RISCV/gem5.opt -d /tmp/icp-feedback tests/gem5/replacement_policies/icp_trace.py --scenario feedback
```

Other scenarios are `no-epf`, `secure`, `demotion`, and `merged`. Hardware-prefetch
trace packets need XiangShan source metadata; TraceGen initializes default
metadata for such packets so the tests traverse the existing prefetch queue.
The first four scenarios were also checked through software-prefetch fills.

Initial remote screening uses `idealkmhv3.py`,
`spec06-rva23-novec-gcc16-0.3c`, all 145 points, explicit
`--warmup-insts-no-switch=20000000 --maxinsts=40000000`, distributed default pool
with 16 jobs/server.
Full 1.0c validation remains pending. No performance claim yet.
