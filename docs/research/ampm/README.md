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
  returned HTTP 403; a public author manuscript is still being sought.

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
experiments. Policy details and validation will be recorded after the
paper's mechanism is confirmed.

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

The AMPM/UMOA candidate is still required; no performance improvement has
been demonstrated. UMOA's 2012 full text and source have not been obtained.
Unpaywall additionally reports no open-access locations; this is an access
limitation, not evidence that no implementation exists.

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
