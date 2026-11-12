# AMPM and UMOA research

CRP Section 5.4.1 (printed page 49) spells the predictor **AMPM**, and
separately cites Ishii et al. 2011 for the prefetcher and Ishii et al. 2012
for its cache insertion extension. The task spells this APMP and also refers
to FDP; clarification is pending. FDP has a separate active implementation.

## Publications

- Yasuo Ishii, Mary Inaba, Kei Hiraki. *Access Map Pattern Matching for High
  Performance Data Cache Prefetch*. JILP 13 (2011), 1–24.
  Official PDF: https://www.jilp.org/vol13/v13paper3.pdf
  Downloaded locally as `ishii-ampm-jilp2011.pdf`.
- The same authors. *Unified Memory Optimizing Architecture: Memory
  Subsystem Control with a Unified Predictor*. ICS 2012, pp. 267–278.
  DOI: https://doi.org/10.1145/2304576.2304614
  This is the actual insertion-control reference in CRP. Publisher PDF
  returned HTTP 403. Public full text was subsequently located at
  https://www.academia.edu/58343593/Unified_memory_optimizing_architecture
  Section 3.3, Table 2, and the following RRIP discussion provide the
  insertion and promotion specification. An actual UMOA PDF download is
  still unavailable; the 2011 AMPM PDF and original source are local.

## Source search

- gem5 already implements the 2011 prefetcher in
  `src/mem/cache/prefetch/access_map_pattern_matching.{hh,cc}`. This is
  Metempsy's implementation; it does not implement the 2012 cache insertion
  extension.
- The official DPC-1 program links the original authors' 2009 source:
  https://www.jilp.org/dpc/online/DPC-1%20Program.htm
  Downloaded the original `code/03_v1_sample_prefetcher.cpp` and `.h`
  into the local, ignored `original-source/` directory. The 2009 write-up
  is also saved as `ishii-ampm-dpc2009.pdf`.
- Original UMOA source has not been found yet. AMPM's prefetcher source
  must not be reported as an implementation of UMOA's cache insertion.

- Related predecessor: *Cache Replacement Policy Using Map-based Adaptive
  Insertion* (JWAC-1/CRC 2010). Official paper and source:
  https://www.jilp.org/jwac-1/online/papers/009_ishii.pdf
  https://www.jilp.org/jwac-1/online/code/009_ishii.tgz
  Paper downloaded as `ishii-maip-crc2010.pdf`. MAIP is not UMOA;
  the 2010 paper explicitly leaves interaction with prefetching to future work.
- ACM's normal, legacy, and epdf links returned HTTP 403 for UMOA.
  Semantic Scholar and OpenAlex expose no open-access copy. The University
  of Tokyo repository returns HTTP 503 with a maintenance notice; these
  access failures do not demonstrate that no source exists.

## Experiment scope

Use the isolated `ampm-l2` worktree, based on `f9649a04ee`, for L2-only
insertion controls, LRU and XSDRRIP comparisons, and remote manual-perf
experiments. The PA-MAIP insertion/promotion subset is implemented below;
performance validation is still in progress.

## Baseline setup validation

`--l2-replacement-policy=lru|xsdrrip` applies after KMHv3 configuration
initialization, to every classic cache or wrapper slice. Default behavior
is preserved when the override is absent. XSDRRIP geometry is derived from
the actual cache size, associativity, and cache line size.

Validated on 2026-11-13:

- Python syntax checks for all changed configuration modules.
- Real gem5 SimObject configuration checks: classic/sliced L2, default/LRU/
  XSDRRIP, and 512 KiB / 8-way / 64-byte geometry (1024 sets).
- `util/style.py --modifications` for the changed Python files: passed.
- Optimized build in `/tmp/apmp-build/build/RISCV`: passed. The local
  worktree links `ext/dramsim3/DRAMsim3` to the read-only shared CI dependency
  at `/nfs/home/share/gem5_ci/DRAMsim3`.
- Full-system `gcb_test.zstd` smoke tests with DRAMSim3 and NEMU difftest:
  LRU and XSDRRIP both exited normally after 100,000 committed instructions.
  Both reported 56,368,242 simTicks; this small checkpoint does not establish
  a performance trend. All four L2 slice policies were verified in config.ini.
  L1D remained TreePLRU and L3 remained LRU in both configurations.

The PA-MAIP candidate now has source code in this worktree; build and
behavioral validation are in progress. No performance improvement has
been demonstrated. UMOA's original source has not been found. Unpaywall
reported no open-access locations, but the public full-text page above
subsequently supplied the algorithm specification.

## Remote experiment ledger

Fixed tested commit: `a61859f355cf013aa6c15f95b6be0fe24b829976`.
Workflow ref: `ampm-l2`. Configuration: `idealkmhv3.py`.
Suite: full `spec06-rva23-novec-gcc16-1.0c`, no benchmark subset.
Distributed servers: `default`; 32 jobs per server. All prefetcher and other
machine settings use the configuration defaults.

| Policy | Run | URL | Last verified result |
| --- | --- | --- | --- |
| LRU | 43271285986 | https://github.com/OpenXiangShan/GEM5/actions/runs/43271285986 | Created; final result pending |
| XSDRRIP | 43271296003 | https://github.com/OpenXiangShan/GEM5/actions/runs/43271296003 | Created; final result pending |

Only the extra argument differs: `--l2-replacement-policy=lru` versus
`--l2-replacement-policy=xsdrrip`.

Initial runs 43270560390 / 43270568832 failed at checkout because the short
SHA was interpreted as a branch. Neither compiled or simulated; exclude
both runs from all performance comparisons. Corrected runs above use the
full 40-character SHA.

Reproduce the local smoke from the worktree with:

```sh
GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so \
/tmp/apmp-build/build/RISCV/gem5.opt --outdir=/tmp/apmp-smoke-lru-dram \
  configs/example/idealkmhv3.py \
  --generic-rv-cpt=/nfs/home/share/gem5_ci/checkpoints/gcb_test.zstd \
  --maxinsts=100000 --warmup-insts-no-switch=0 --l2-replacement-policy=lru
```

Use a separate output directory and `--l2-replacement-policy=xsdrrip` for
the second policy. The checkpoint uses its embedded restorer.

## L2 PA-MAIP implementation

The selected subset is UMOA Section 3.3's map-state insertion and promotion
policy. The unmodified prefetchers still generate exactly their existing
requests. This is not a complete MUMSC implementation: IP-Table bypass,
DRAM scheduling, and whole-system predictor placement are outside scope.
Table 2's normal-IP insertion column is the implemented starting point.

- Each core has one `AMPMHintTable` shared by all L2 slices: 256 entries,
  16 ways, 4 KiB zones and 64-byte lines. Four map states distinguish Init,
  Prefetched, Accessed and Reused (the paper's Accessed*).
- A prefetch moves Init to Prefetched; its first demand consumption moves
  to Accessed. A later demand reference moves Accessed to Reused. Prefetch
  references never manufacture temporal reuse.
- Requests snapshot the **previous** map state before training. Init and
  Prefetched insert at RRPV 3; Accessed and Reused insert at RRPV 2.
  One in every 32 low-priority insertions gets RRPV 2 (a local deterministic
  BIP counter). Demand fills do not consult future accesses at refill time.
- Prefetch tag hits do not promote. A per-line unused-prefetch bit suppresses
  promotion for the first demand hit; later demand hits promote to RRPV 0.
  A demand merged into an in-flight prefetch consumes that first-use bit
  directly at fill without falsely promoting the newly inserted block.
- Repeated lookups of the same request reuse its snapshot. The bounded
  annotation is owned by `Request`, including its copy/reuse lifecycle;
  there is no unbounded outstanding-request table in the policy.
- Writebacks have no demand map hint and use ordinary SRRIP insertion at
  RRPV 2. Valid victims use RRIP aging; invalid entries win before aging.

Adaptation boundary: the paper observes private-cache references with a
co-designed AMPM predictor and controls L3. Here the map observes L2 traffic
and controls L2, as requested. References absorbed by L1 are not visible.
The request annotation carries two map-state bits; its software owner
pointer deduplicates cache observation and is not modeled hardware storage.
No extra modeled latency is added. A lookup probes at most 16 map entries;
a map replacement clears 64 bounded states; victim selection scans the
cache ways. Default baseline policies receive no-op callbacks.

Configuration: `--l2-replacement-policy=ampm`. Optional experiment controls
are `--ampm-map-entries`, `--ampm-map-assoc`, `--ampm-low-insert-period` and
`--ampm-promote-first-hit` (insertion-only ablation).

Validation plan: targeted state/first-consumption unit tests, rebuilt opt
binary, unchanged LRU/XSDRRIP smoke statistics with the added no-op hooks,
PA-MAIP checkpoint difftest and counter checks, then remote full SPEC06
comparison to the fixed baselines. Regressions will be analyzed with demand
misses, map-state distributions, insertion RRPV and promotion statistics.

## Initial L2-map validation (dd595ca9d8)

- Seven access-map/line-state tests and three actual `Request` ownership,
  copying and virtual-request-reuse tests passed.
- All three policies passed the 100K-instruction DRAMSim3/NEMU smoke. With
  the new no-op observation hooks, **all** common non-host statistics match
  the previous LRU/XSDRRIP outputs: 11,224 / 11,244 common statistics.
- The first SPEC mcf attempt used the old shared `ref/normal` library and
  failed at the embedded restorer's `mstateen0` instruction for all three
  policies, at the same tick. Exclude these attempts from performance data.
- Corrected mcf tests use the checked release returned by
  `python3 util/nemu_ref/resolve.py normal-dedup`, namely release
  `af9d4ce51fc2-gem5-r4-multi16g-zfa-cbo-sstc`, with `--enable-mem-dedup`.
  All three exited normally with difftest enabled.

Local point: `mcf/12886/_12886_0.149435_memory_.zstd`, 4M total instructions,
zero warmup. This is a cold short smoke, **not** a weighted SPEC result.

| Policy | IPC | L2 read/write-command misses (includes upstream PF) | simTicks |
| --- | ---: | ---: | ---: |
| LRU | 0.600611 | 249674 | 2217745701 |
| XSDRRIP | 0.603305 | 247161 | 2207842947 |
| L2-map PA-MAIP | 0.601034 | 255653 | 2216180601 |

PA-MAIP is about +0.07% IPC versus LRU and -0.38% versus XSDRRIP here.
Instruction-limit overshoot differs by eight instructions; compare IPC,
not an exact instruction-matched runtime. The generic `demandMisses::total`
formula includes upstream prefetchers' ReadSharedReq commands. Splitting
by requestor gives genuine CPU-data misses of 186326 (LRU), 178463
(XSDRRIP), and 165764 (PA-MAIP). Thus CPU-data misses decrease by 7.1%
versus XSDRRIP while IPC is slightly worse. The aggregate increase is
from prefetch requests; increased prefetch traffic, miss latency and
resource waits need investigation before attributing a cause.

Full default-prefetcher PA-MAIP control run:
https://github.com/OpenXiangShan/GEM5/actions/runs/43279634993
Tested commit: `dd595ca9d8e268cf4d8a2c0e83fd4cf7047095f9`.
Same full SPEC06 configuration as runs 43271285986 / 43271296003; only
`--l2-replacement-policy=ampm` differs. Final status/score pending.

## Real AMPM state provider

The L2-map reconstruction above is kept as a separate control. To exercise
the paper's actual cooperative information source, an optional receiver is
attached to the existing AMPM prefetcher. It reads the real map before
training a demand or changing a target to Prefetched. Demand requests and
both physical/translated prefetch queue entries carry that snapshot to L2.
The receiver allocates no second access-map table in this mode.

Use `--ampm-hint-source=prefetcher --l1d-hwp-type=AMPMPrefetcher` together
with each of `--l2-replacement-policy=lru|xsdrrip|ampm`. All three must use
identical AMPM parameters. The helper sets 256 maps, 16 ways and 4 KiB zones,
physical-address training on all data accesses, and unchanged degree-control
thresholds. These runs require their own baselines; default XSComposite
baselines cannot establish a cooperative-AMPM speedup.

The existing AMPM has three stored states. For Table 2's selected normal-IP
column, Accessed and Accessed* have the same insertion priority, so the
three-state provider is sufficient. An earlier demand use absorbed by L1
can clear the shared cache's otherwise stale unused-prefetch indication.
Unannotated instruction-cache/page-table traffic falls back to ordinary
SRRIP insertion rather than inventing predictor knowledge.

Required compatibility fixes for this AMPM path are applied identically in
all comparisons: tag AMPM candidates with a distinct source (the old
source-less constructor panics), do not wrap an unsigned degree below zero,
and honor a zero candidate budget. These are controller correctness fixes,
not parameter tuning to compensate for replacement regressions.

A `prefetchHintFills` counter checks that issued prefetch metadata survives
the queue/translation path. In external mode, `prefetchStates` counts
candidate hints before queue filtering; `prefetchFills` counts actual fills.

Real-provider validation so far:

- `gem5.fast` build and all ten unit tests passed.
- Three 100K-instruction gcb_test.zstd smokes passed with the pinned REF.
  Configs confirm identical AMPM geometry and training in all three cases.
- A configuration mismatch in the legacy `MemorySize` entry-count parameter
  was fixed with string conversion before successful runs.
- The generic configuration enabled PFRequestBuffer for AMPM, but AMPM
  returns vector candidates. Disable **only AMPM's** buffer mode in all
  three controls. Otherwise it generates hints but issues no prefetches.
- After this interface fix, 13 AMPM prefetches reached the issue boundary;
  all 13 corresponding L2 PA-MAIP fills retained their map-state hints.
  Other L2 worker prefetches remain unannotated and use the documented
  ordinary fallback.
- Real-provider 4M-instruction mcf comparisons passed functional checks;
  the corrected results are below. Full coupled SPEC06 runs are pending.

Default-prefetcher baseline archives were identified from their actual
`metadata.txt` workflow_run_id fields while the jobs were live (the normal
CI log locator returned HTTP 404 for unfinished logs):

- LRU 43271285986:
  `/nfs/home/share/gem5_ci/performance_data/spec06-rva23-novec-gcc16-1.0c/20261113_014415_a61859f355_idealkmhv3_run2118`
- XSDRRIP 43271296003:
  `/nfs/home/share/gem5_ci/performance_data/spec06-rva23-novec-gcc16-1.0c/20261113_014354_a61859f355_idealkmhv3_run2119`

Both list 1094 checkpoints and the same pinned normal-dedup REF. Many
stats files exist, but no final score/conclusion was available at the last
check. File existence alone does not prove that a point completed.

### AMPM monotonic-state correction before final controls

Further inspection of JILP 2011 Section 3.1 and the downloaded author
source (`MemoryAccessMapTable::IsCandidate`) confirms that only Init
positions may generate prefetches. The repository's prior `tgt != AM_INVALID`
test also selected Accessed/Prefetched positions, then rewrote their states
to Prefetched. This violates the paper's unidirectional state transitions
and corrupts the first-use information consumed by PA-MAIP.

The provider now requires `tgt == AM_INIT`, and asserts that only Init can
transition to Prefetched. This prerequisite is identical in every final
cooperative-AMPM control; it is not a replacement-only speedup claim.

Before correcting this legacy target predicate, the 4M mcf test passed
functional checks but PA-MAIP IPC was 0.770226 versus 0.828631 LRU and
0.823594 XSDRRIP. AMPM issued only 42 prefetches; all 42 carried hints to
L2. PA-MAIP's genuine CPU-data misses were 138633 versus 160054 XSDRRIP,
but mean L2 CPU-data miss latency rose to 34647 ticks versus 27998.
These are diagnostic pre-correction results, not the final AMPM controls.

The corrected three-policy mcf test uses output prefix
`/tmp/apmp-coupled-correct-mcf4m-`. All three exited normally with difftest
enabled. All candidate hints were Init; none were Prefetched or Accessed.
AMPM issued 43 prefetches and all 43 PA-MAIP prefetch fills retained hints.

| Policy | IPC | L2 CPU-data misses | Mean L2 CPU-data miss ticks |
| --- | ---: | ---: | ---: |
| LRU | 0.828631 | 140443 | 29292 |
| XSDRRIP | 0.821380 | 160257 | 27931 |
| PA-MAIP | 0.771002 | 139807 | 34706 |

The state correction does not remove the regression: PA-MAIP is -6.95%
versus LRU and -6.13% versus XSDRRIP. PA-MAIP suppresses promotion for
207177 first uses, predominantly of other L2 worker prefetches without an
AMPM hint. Lower miss count is accompanied by higher miss latency; a
miss-count improvement alone is insufficient. An insertion-only ablation
(`--ampm-promote-first-hit`) will separate this effect from demand insertion
priority. These cold 4M results are mechanism diagnostics, not suite scores.
Results are archived in `mcf4m-ampm-corrected.json`.
