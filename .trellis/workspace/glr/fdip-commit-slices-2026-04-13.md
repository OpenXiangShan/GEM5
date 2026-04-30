# FDIP Commit Slice Plan (2026-04-13)

## Goal

Turn the current dirty `fdip-phase2-xsdev` worktree into reviewable slices
without mixing:

- predictor/config semantics,
- fetch runtime behavior,
- cache-side metadata / refill gating,
- local Trellis notes.

## Recommended slice order

### Slice 1: predictor + config semantics + coverage-helper witness

Focus:

- define the user-visible FDIP knobs and predictor-side semantics;
- keep FDIP-off behavior unchanged;
- lock the actual-coverage helper witness.

Files:

- `src/cpu/pred/BranchPredictor.py`
- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/decoupled_bpred_stats.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/test/fetch_coverage.test.cc`
- `configs/common/Options.py`
- `configs/common/xiangshan.py`

Key points:

- `bpu_runahead_entries` only constrains behavior when FDIP is enabled;
- `MicroTAGE` inherited params (`blockSize`, `numDelay`) use plain assignment;
- cross-boundary control-tail coverage is covered by helper-level witness.

Review note:

- this slice is independently stageable, but the witness here is only the
  helper-level proof. The runtime fetch-side consumer still lives in slice 3.

Suggested commit shape:

- tags: `cpu-pred,cpu-o3,tests`
- title idea: `cpu-pred,cpu-o3,tests: Tighten FDIP predictor semantics`

### Slice 2: request/cache contract + refill gating substrate

Focus:

- carry FDIP metadata through request / cache pipeline;
- define the cache-side helper contract used by fetch direct-probe /
  recent-unused paths;
- implement old-path refill drop and recent-unused suppress interface.

Files:

- `src/mem/request.hh`
- `src/mem/cache/base.hh`
- `src/mem/cache/base.cc`
- `src/mem/cache/cache.cc`
- `src/mem/cache/cache_probe_arg.hh`
- `src/mem/cache/xs_l2/SlicedCacheAccessor.hh`
- `src/mem/cache/xs_l2/SlicedCacheAccessor.cc`
- `src/mem/ruby/structures/RubyPrefetcherProxy.hh`

Key points:

- epoch-mismatched FDIP refills are observable and droppable;
- recent-unused suppression keys off `(blkAddr, isSecure)` physical-line identity;
- selected-way hint contract is introduced here as shared substrate, not as a
  pure cache-internal detail.

Review note:

- this slice is the producer/consumer contract layer for slice 3;
- it is cleaner as a preceding slice than as a trailing “cache-only” chunk.

Suggested commit shape:

- tags: `mem-cache,cpu-o3`
- title idea: `mem-cache,cpu-o3: Add FDIP cache contract and refill gating`

### Slice 3: fetch-side FDIP engine + direct-probe consumer

Focus:

- fetch-local FDIP scheduling and per-entry line derivation;
- direct ICache probe hit handling and selected-way hint consumption;
- wrong-path accounting and trace-mode ICache stall observability.

Files:

- `src/cpu/o3/fetch.hh`
- `src/cpu/o3/fetch.cc`
- `src/cpu/o3/BaseO3CPU.py`
- `configs/example/kmhv3.py`

Key points:

- FTQ-directed line generation uses actual request coverage;
- direct probe can terminate FDIP work early without allocating a miss;
- wrong-path issue / demand-reuse accounting stays fetch-local;
- trace-mode `fetch.icacheStallCycles` accounting is fixed here;
- this slice depends on slice 2’s request/cache metadata contract.

Suggested commit shape:

- tags: `cpu-o3`
- title idea: `cpu-o3: Extend FDIP fetch engine with probe and tracking`

## Notes that should stay out of code commits

- `.trellis/workspace/glr/fdip-p0-analysis-2026-04-13.md`
- `.trellis/workspace/glr/fdip-commit-slices-2026-04-13.md`
- `.trellis/workspace/glr/wave1-execution-checklists-2026-04-13.md`

These are useful working notes, but they are not part of the upstream code
change unless we later decide to promote them into project docs.

## Validation to preserve after slicing

- `openspec validate add-fdip-icache-prefetch --strict`
- `scons build/RISCV/gem5.opt -j8`
- `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`
- summarized `srv67` smoke / 5M conclusions in
  `.trellis/workspace/glr/fdip-p0-analysis-2026-04-13.md`
- summarized high-I$ sanity conclusions in
  `.trellis/workspace/glr/fdip-p0-analysis-2026-04-13.md`
