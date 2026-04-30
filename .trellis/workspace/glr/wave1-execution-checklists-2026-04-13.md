# Wave 1 Execution Checklists (2026-04-13)

## Scope

Wave 1 covers the three ready-parallel tasks in the current DAG:

1. `04-02-openspec-add-fdip-icache-prefetch`
2. `04-02-openspec-add-instruction-prefetch-research-framework`
3. `04-02-openspec-add-frontend-sram-sharing-feasibility`

The goal of this note is to define:

- current status,
- the first implementation slice,
- handoff-friendly execution checklists,
- minimal validation gates.

## 1. FDIP (`add-fdip-icache-prefetch`)

### Current status

- Trellis status is `in_progress`, branch is `fdip-phase2-xsdev`, worktree is `.worktrees/fdip-phase2-xsdev`.
- Imported OpenSpec progress is `38 / 116`.
- The branch already contains two feature commits:
  - `a3c8636b34` `cpu-o3,bpu,mem-cache: Add FDIP baseline plumbing`
  - `9182ec2779` `mem-cache,arch-riscv: Add FDIP L1I miss gating`
- The worktree is still dirty, with 18 modified files and one untracked `.tmp/`.

### What is already present

- Config/plumbing exists for FDIP parameters:
  - `src/cpu/pred/BranchPredictor.py`
  - `configs/common/xiangshan.py`
  - `configs/common/Options.py`
  - `configs/example/kmhv3.py`
- Predictor-side knobs already exist in `DecoupledBPUWithBTB`.
- Fetch-side FDIP engine scaffolding already exists in `src/cpu/o3/fetch.cc` / `fetch.hh`.
- Cache/request-side metadata, gating, and feedback stats are already present in:
  - `src/mem/request.hh`
  - `src/mem/cache/base.hh`
  - `src/mem/cache/base.cc`
  - `src/mem/cache/cache_probe_arg.hh`
  - `src/mem/cache/xs_l2/SlicedCacheAccessor.*`

### Remaining shape

The task is not at "start from zero". It is at "turn a dirty research branch into coherent slices":

- baseline plumbing is committed,
- P0 mitigation logic is mostly present in the dirty worktree,
- the main gaps are closure, witness coverage, and commit slicing.

### FDIP execution checklist

#### Slice A: stabilize current dirty branch

- Reconcile the dirty worktree into 2-3 commit-sized chunks:
  - fetch/predictor scheduler behavior
  - cache-side gating and stats
  - config/docs/validation updates
- Remove or classify `.tmp/` before commit planning.
- Confirm no unrelated edits are mixed into the branch.

#### Slice B: finish Phase-1/P0 closure

- Re-check that current behavior matches post-`control-pc-view` actual fetch coverage semantics.
- Add or refresh witness coverage for:
  - actual coverage span calculation
  - cross-boundary 4B control-tail line coverage
  - redirect / partial-state cleanup
- Close the remaining focused smoke gap noted in OpenSpec tasks.

#### Slice C: turn research branch into reviewable deliverable

- Separate "must-have P0" from "optional Phase 2/3 exploration".
- Update docs for parameter semantics and known limitations.
- Refresh validation outputs and attach an analysis note for the current tuned configuration.

### Minimal validation gate

- `openspec validate add-fdip-icache-prefetch --strict`
- `build/RISCV/gem5.opt`
- one FDIP-off smoke
- one FDIP-on smoke
- focused `srv67` comparison with current P0 config
- at least one witness for cross-boundary control-tail coverage

### Validation snapshot (2026-04-13)

- `openspec validate add-fdip-icache-prefetch --strict` passes on the current worktree.
- `scons build/RISCV/gem5.opt -j8` passes after the current FDIP/fetch/predictor changes.
- Focused smoke / `srv67` 5M results have been summarized into Trellis-owned
  notes and do not need the original transient `.tmp/` artifact directories.
- Smoke (`srv67`, 500k / warmup 100k) vs existing anchor:
  - `fdipMissAllocAfterUnusedThenUnused`: `3258 -> 46` (`-98.59%`)
  - `fdipMissAllocAfterUnusedThenUseful`: `240 -> 17` (`-92.92%`)
  - `fdipDroppedRefill`: `0 -> 7420`
  - `fdipFilteredRecentUnused`: `0 -> 734`
  - `IPC`: `1.530011 -> 1.529824` (`-0.01%`)
- `srv67` 5M vs existing anchor:
  - `fdipMissAllocAfterUnusedThenUnused`: `57111 -> 2116` (`-96.29%`)
  - `fdipMissAllocAfterUnusedThenUseful`: `1525 -> 303` (`-80.13%`)
  - `fdipDroppedRefill`: `0 -> 71580`
  - `fdipFilteredRecentUnused`: `0 -> 5084`
  - `IPC`: `1.817250 -> 1.824324` (`+0.39%`)
  - `overallAvgMissLatency::total`: `517.72 -> 310.22` (`-40.08%`)
- Validation-side fix applied during this pass:
  - `MicroTAGE` no longer shadows inherited `TimedBaseBTBPredictor.blockSize/numDelay`; smoke stats stayed bit-identical before/after the fix, and the prior `blockSize=0` warning disappeared.
- Interpretation:
  - current benefit comes mainly from suppressing repeated bad FDIP lines and dropping stale refill pollution;
  - useful-hit volume is still lower than the historical anchor, but miss latency improves enough for `srv67` 5M IPC to move slightly positive.

## 2. Minimal Research Framework (`add-instruction-prefetch-research-framework`)

### Current status

- Trellis status is `planning`, progress is `0 / 28`.
- Proposal/design/spec/validation files already exist.
- No runtime implementation symbols were found in the current codebase.

### Guiding constraint

Do not start with a giant universal framework. Wave 1 should be a minimal runtime bring-up only.

### Research framework execution checklist

#### Slice A: lock Wave-1 scope

- Limit Wave 1 to:
  - runtime enable/disable params
  - one policy-agnostic observer
  - one shared submitter/scoreboard skeleton
  - one null/no-op policy
- Explicitly defer:
  - bytes service
  - timing/history service
  - full path-epoch service
  - BTB sink / rich cache adapters

#### Slice B: minimal event path

- Add the smallest normalized observer path:
  - demand-block event from O3 fetch
  - trace-side equivalent demand event
- Keep the event ABI policy-agnostic.
- Do not add policy-specific callbacks in the first slice.

#### Slice C: runtime skeleton

- Add runtime params in `src/cpu/o3/BaseO3CPU.py`.
- Add a frontend-owned runtime sidecar under `src/cpu/o3/`.
- Add trace adapter glue under `src/cpu/o3/trace/`.
- Register new files and tests in `src/cpu/o3/SConscript`.

#### Slice D: null-policy bring-up

- Make null policy loadable and observable.
- Ensure it emits zero candidates and does not change architectural behavior.
- Add scoreboard stats so the runtime is observable even with the null policy.

### Minimal validation gate

- `openspec validate add-instruction-prefetch-research-framework --strict`
- `build/RISCV/gem5.opt`
- one runtime-only smoke with null policy
- one trace smoke through the trace adapter
- confirm scoreboard/runtime stats are present

## 3. SRAM Feasibility (`add-frontend-sram-sharing-feasibility`)

### Current status

- Trellis status is `planning`, progress is `0 / 26`.
- Proposal/design/spec/tasks already exist.
- This is analysis-only in Wave 1, not a runtime microarchitecture change.
- The hard prerequisite is a stable post-control-PC frontend baseline with the required ArchDB tables.

### Main risk

The first blocker is not the analysis script. It is baseline availability:

- confirm the correct worktree/branch can generate `FrontendPrefetchBTBBranchAccess`
- confirm it can generate `FrontendPrefetchICacheAccess`
- do not mix pre-/post-fetch-range-alignment DBs

### SRAM feasibility execution checklist

#### Slice A: lock the baseline

- Select one post-control-PC frontend baseline.
- Record:
  - `git rev-parse HEAD`
  - `frontend_semantics_tag`
  - exact ArchDB enable command line
- Verify the selected baseline can emit the required DB tables.

#### Slice B: trace driver opt-in plumbing

- Extend `util/xs_scripts/trace/run_trace_champsim.sh` with opt-in extra args.
- Keep default behavior unchanged when extra args are unset.
- Run one short smoke and confirm the SQLite DB is generated and non-empty.

#### Slice C: minimum offline analysis tool

- Add `util/xs_scripts/trace/analyze_frontend_sram_sharing.py`.
- First slice supports:
  - schema validation
  - stable sorting
  - `correctPath` filter
  - `tid` filter
  - windowing
  - per-window sample counts
  - BTB miss-rate vs capacity
  - ICache miss-rate vs capacity
  - `need_btb` / `need_icache`
- Keep dependencies to stdlib only.

#### Slice D: second-pass research outputs

- Add direction-predictor proxy model.
- Add shared-SRAM lower-bound summary.
- Add sensitivity sweep and deterministic reporting.
- Add pyunit coverage for LRU/windowing/schema validation.

### Minimal validation gate

- `openspec validate add-frontend-sram-sharing-feasibility --strict`
- one short trace smoke that produces a non-empty SQLite DB
- pyunit for core analysis logic
- deterministic rerun on the same DB

## Recommended first parallel wave

Run these three in parallel, but not at the same risk level:

- `FDIP`: main implementation line, because it already has real code and a dirty worktree to converge.
- `Research framework`: clean-room minimal runtime bring-up on a fresh worktree.
- `SRAM feasibility`: analysis-only line on a fresh worktree, but start with baseline confirmation before writing the tool.

## Recommended ownership split

- Owner A: FDIP stabilization and commit slicing on `.worktrees/fdip-phase2-xsdev`
- Owner B: minimal runtime framework bring-up on a new clean worktree
- Owner C: SRAM feasibility baseline + offline analysis tooling on a new clean worktree

## Suggested stop/go rules

- If FDIP cannot be cleanly split from unrelated dirty edits, stop and re-scope before adding more logic.
- If the research framework starts pulling in bytes/timing/path services in slice 1, stop and cut scope.
- If SRAM feasibility cannot find the required ArchDB tables on a clean baseline, stop before writing analysis code and resolve the producer baseline first.
