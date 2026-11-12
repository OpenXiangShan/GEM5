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
experiments. The initial fixed-BRRIP insertion/promotion control is implemented below;
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
| LRU | 43271285986 | https://github.com/OpenXiangShan/GEM5/actions/runs/43271285986 | Success; overall/GHz 22.96817 |
| XSDRRIP | 43271296003 | https://github.com/OpenXiangShan/GEM5/actions/runs/43271296003 | Success; overall/GHz 22.91633 |

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

The first implementation is a fixed-BRRIP, 2-bit control for UMOA Section
3.3's map-state insertion and promotion. Its prefetchers generate their
existing requests. It omits the SRRIP/BRRIP selection used in the full
PA-MAIP paper configuration. The corrected adaptive configuration is
specified below. DRAM scheduling and complete MUMSC placement are outside
scope. Earlier references here to a 'normal-IP column' and IP-Table bypass
were incorrect: Table 2 has SRRIP and BRRIP columns, not IP classes.

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

The existing AMPM has the paper's three stored states. The shadow map's
extra Reused state maps to the same insertion priority as Accessed; it is
a diagnostic distinction, not a fourth predictor state required by UMOA. An earlier demand use absorbed by L1
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


## Completed default-prefetcher controls

Both full 1.0c baseline runs completed successfully with 1094 points and
29 benchmarks. Archived `mcf_6753` configs verify 20M warmup / 40M total,
1024 sets / 8 ways in each L2 slice, and the intended policy in all slices.
Official scores were reproduced with the read-only CI tool at
`/nfs/home/share/gem5_ci/gem5_data_proc/run.py`, with outputs under
`/tmp/apmp-proc-lru` and `/tmp/apmp-proc-xs`. The preferred personal tool
path is inaccessible; no shared files were modified.

| Metric per GHz | LRU | XSDRRIP |
| --- | ---: | ---: |
| Integer | 21.897064 | 21.911378 |
| Floating point | 23.755644 | 23.653340 |
| Overall | 22.968172 | 22.916332 |

XSDRRIP is -0.226% overall relative to LRU; the largest difference is
milc (-4.40%). These controls use XSComposite, not the real AMPM protocol.
The default-prefetcher PA-MAIP run 43279634993 remains in progress and
has a segmentation fault on `cactusADM_16`. It is not a passing performance
result. A local matched LRU/PA-MAIP reproduction is underway.

## Matched cooperative AMPM screening

Fixed commit `6676dc34048d0248e787f7fa73d13ae3b9fc1c55`, workflow ref
`ampm-l2`, `idealkmhv3.py`, full benchmark set of
`spec06-rva23-novec-gcc16-0.3c`, default distributed servers, 32 jobs/server.
Every run adds `--l1d-hwp-type=AMPMPrefetcher --ampm-hint-source=prefetcher`:

| Policy | Run | Result |
| --- | --- | --- |
| LRU | 43288386888 | Created, final result pending |
| XSDRRIP | 43288394351 | Created, final result pending |
| Fixed-BRRIP PA-MAIP (2-bit) | 43288403317 | Canceled while queued; superseded by adaptive configuration |

## L2 demand-protection experiment

The initial fixed-BRRIP control demotes a first demand fill to RRPV 3. On the cold
4M mcf diagnostic, replacing all insertion levels by RRPV 2 recovers most
of its regression, whereas allowing first-use promotion makes it worse.
The optional `--ampm-protect-demand` inserts demand responses at RRPV 0;
map-state insertion still controls prefetch fills. A demand merged with
an in-flight prefetch receives the same protection on consumption at fill.
First demand tag hits retain the paper rule unless the separate first-hit
ablation is requested. The option defaults off and adds no predictor state.

The refill path passes responses, so the experiment explicitly recognizes
ReadResp/WriteResp as demand fills instead of relying on MemCmd::isDemand,
which lists only requests. `protectedDemandFills` and `insertions::0` verify
that the option takes effect. The first diagnostic with a request-only
predicate did not enable demand insertion and is excluded.

| mcf cold 4M variant | IPC |
| --- | ---: |
| LRU | 0.828631 |
| XSDRRIP | 0.821380 |
| Fixed-BRRIP PA-MAIP (2-bit) | 0.771002 |
| Paper, promote first hit | 0.735215 |
| All RRPV 2, suppress first use | 0.824147 |
| All RRPV 2, promote first use | 0.822649 |
| Demand protection, suppress first use | 0.828722 |
| Demand protection, promote first use | 0.823462 |

All listed runs exited normally with difftest. The final demand-protection
run recorded 118261 immediate-priority insertions plus 1034 merged
prefetch consumptions; all 43 AMPM prefetch fills retained hints. Its IPC
is +0.011% vs LRU and +0.894% vs XSDRRIP. This diagnoses the L2 adaptation;
it does not establish a weighted suite gain. The extra demand-protection
counter and response predicate need broader point validation before use
in a final performance candidate.


## Corrected adaptive paper configuration

A second check of the complete UMOA Section 3.3/Table 2 exposed an omission
in the initial experiment: the paper selects SRRIP or BRRIP with set
dueling, and evaluates 3-bit RRPV. The initial fixed-BRRIP, 2-bit runs are
valid ablations but do not establish the performance of full PA-MAIP.
The 'normal-IP' wording used earlier was an incorrect reading and has
been removed. The distinct 2010 MAIP predecessor must not be conflated
with this 2012 table.

The corrected Table 2 implementation is selected explicitly:

```sh
--l2-replacement-policy=ampm --ampm-insertion-mode=drrip --ampm-rrpv-bits=3
```

For the real AMPM protocol, add the same
`--l1d-hwp-type=AMPMPrefetcher --ampm-hint-source=prefetcher` to every
comparison policy. Defaults retain the initial fixed-BRRIP, 2-bit control
so existing commands and ongoing experiments keep their meaning.

- SRRIP inserts all map states at RRPV 6.
- BRRIP inserts Init/Prefetched at RRPV 7, with one in 32 at RRPV 6;
  Accessed (including the shadow map's Reused diagnostic) inserts at 6.
- A 10-bit saturating selector favors the leader with fewer allocations.
  At 1024 sets, 32 disjoint sets lead each policy and 960 follow the
  selector. Set roles are computed once per entry. Selector access and
  updates are constant-time; victim selection still scans only the ways.
- Prefetch hits and first demand consumption do not promote; temporal
  demand reuse promotes to RRPV 0. The existing demand-protection and
  first-hit options remain explicit L2 adaptation experiments.
- Missing predictor hints use the ordinary SRRIP insertion level, 6 for
  3-bit RRPV. This is an adaptation for other prefetch workers and traffic
  absent from the original cooperative predictor, not a paper claim.

The per-slice selector and leader allocation counts are exported alongside
follower SRRIP/BRRIP fills and insertion RRPVs. New selector tests verify
balanced/disjoint leaders, the feedback direction, saturation, and that
followers do not train. Build, state-machine tests, small real-AMPM
transport checks, and warmed point comparisons precede corrected remote
runs. Earlier fixed-BRRIP runs retain their own ledger and result labels.


Adaptive-policy validation (local): the fast build and all 13 unit tests
passed. A 100K gcb_test.zstd run with real AMPM and difftest exited normally.
All four slices report insertion_mode=2, num_bits=3, 1024 sets and 8 ways.
Both RRPV 6 and 7 insertions occur; all 13 issued AMPM prefetches retain
hints at L2. Both leader teams and both follower choices record fills.
Warmed point validation and the corrected Cactus reproducer are running.


## Corrected remote screens

Tested commit `ffb2aa580f1f7f27e3ecf42c50160405eeb76814`:

- Real AMPM adaptive 3-bit: run **43295082033**.
- Default-prefetcher/L2-map adaptive 3-bit: run **43295089737**.

Both use idealkmhv3, all benchmarks in the standard 0.3c point set, default
servers and 32 jobs/server. Dispatch and fixed heads were verified; final
results are pending. The still-queued fixed-BRRIP real-AMPM candidate
43288403317 was canceled and is excluded from performance results.

The 0.3c set contains 145 checkpoints. Its `checkpoints_cov0.3.json` point
IDs exactly match the archived checkpoint list of real-AMPM LRU run
43288386888. The completed default-prefetcher 1.0c baseline CSVs were
filtered to precisely those IDs and reweighted with that JSON. The 0.3c
baseline overall/GHz scores are 24.251479 (LRU) and 24.174477 (XSDRRIP).
These are matched-point baselines; they are not the full 1.0c scores.

## Cactus express-snoop protocol failure

The original fixed-BRRIP default-prefetcher run reproducibly crashes on
`cactusADM_16`. Local LRU succeeds for the same 20M warmup / 40M total and
matches **all 13445 common non-host statistics** of the completed remote
LRU point. The local fixed-BRRIP run reproduces the remote crash, and the
assertion-enabled debugger identifies a missing `CoherentXBar::routeTo`
entry on a snoop response.

A narrow trace locates the cause at tick 4080280968, address 0x82e6a340:
L1D's CMC prefetch misses, the DTB walker cache holds a dirty Owned line
and commits to a response, and the crossbar forwards an express snoop.
The L2 wrapper rejects it under prefetch backpressure, so the crossbar
never installs its return route. The promised response arrives at tick
4080281634 and fails the route lookup. The policy exposes a protocol bug;
the missing route is not fixed by dropping the response or disabling
difftest.

The local fix allows express snoops past the wrapper's prefetch blockage,
and moves the slice's ordinary-request retry assertion after its express
snoop bypass. Successful forwarding retains the existing prefetch-event
scheduling. Unexpected rejection by the downstream slice is fatal rather
than silently corrupting the protocol. The failing fixed-BRRIP point and matched LRU now both pass the complete
40M-instruction run with 20M warmup and difftest. An assertion-enabled
23M-instruction run also passes beyond the original failure. The fixed
BRRIP candidate measures IPC 2.615483 versus LRU 2.619233 on that point.
All 13505 common non-host LRU statistics match the pre-fix local control.
The five warmed real-AMPM workloads also retain every common non-host
LRU/XSDRRIP statistic across the protocol fix (ten additional comparisons).
Broader final performance results must identify their source version;
runs pinned to ffb2aa remain pre-fix experiments.


Real-AMPM LRU screen 43288386888 completed successfully: all 145 points
finished and the official overall/GHz score is **22.731668** (integer
23.229888, floating point 22.386430). It has its own AMPM-prefetcher
protocol and must be compared against the matched AMPM XSDRRIP and
PA-MAIP screens, not against the XSComposite baseline scores.
