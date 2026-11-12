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
- Fresh optimized build in `/tmp/apmp-build/build/RISCV`: in progress.

Remote baselines and the implemented AMPM/UMOA candidate are still required;
no performance improvement has been demonstrated.
