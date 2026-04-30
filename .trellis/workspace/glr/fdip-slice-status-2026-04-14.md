# FDIP Slice Status (2026-04-14)

## Current status

This note consolidates the current best-known stageability status of the FDIP
slice bundles under:

- `.tmp/fdip_slice_bundles_20260413/`

It combines:

- local bundle/apply checks,
- main-worktree build status,
- per-slice subagent review outcomes.

## Commit Snapshot

As of `2026-04-17`, the main worktree has progressed past staged preview and
the three task slices are now committed on `fdip-phase2-xsdev`:

1. `917eb08d37` `cpu,cpu-o3,tests: Tighten FDIP predictor semantics`
2. `5925a5da8c` `mem-cache,cpu-o3: Add FDIP cache contract and refill gating`
3. `ce57b30b6c` `cpu-o3: Extend FDIP fetch engine with probe and tracking`

Current working-tree state:

- no tracked modifications remain for this task stack
- only `.tmp/` artifacts remain untracked

## Slice 1

Scope:

- predictor/config semantics
- coverage-helper witness prep

Current status:

- patch bundle exists and applies cleanly by bundle metadata
- independently stageable in intent
- no fresh isolated full build was successfully recorded on `2026-04-14`

Interpretation:

- safe to treat as a standalone review chunk
- do not overclaim it as a full runtime fetch-coverage proof

Residual risk:

- no narrow slice-local regression yet for “FDIP-off keeps old
  `bpu_runahead_entries` behavior”

## Slice 2

Scope:

- request/cache contract
- old-path refill gating substrate
- `(blkAddr, isSecure)` lifecycle tracking

Current status:

- patch bundle exists and applies cleanly by bundle metadata
- high-confidence independently stageable substrate slice
- main worktree builds cleanly after reviewer-driven fixes

Interpretation:

- acceptable as a standalone base commit
- commit message should describe it as substrate/contract, not full FDIP

Residual risk:

- no fresh isolated full build log captured from a clean temp worktree after
  the last reviewer-driven fixes
- slice-local directed tests remain weaker than integrated trace evidence

## Slice 3

Scope:

- fetch-side FDIP consumer
- direct-probe / selected-way consumption
- wrong-path accounting
- trace-mode `fetch.icacheStallCycles` observability

Current status:

- patch bundle exists and applies cleanly only as `slice2 + slice3`
- should not be reviewed or staged alone
- build-viable is supported by main-worktree integrated build, not by a fresh
  isolated stacked build log

Interpretation:

- review strictly atop `slice 2`
- submit as the consumer follow-up commit, not an independent chunk

Residual risk:

- no fresh isolated `slice2 + slice3` full build log captured after latest
  fixes
- correctness evidence still relies mostly on integrated trace/stats runs

## Recommended order

1. `slice 1`
2. `slice 2`
3. `slice 3` on top of `slice 2`

## Current evidence quality

Strong:

- main-worktree `gem5.opt` build passes
- `openspec validate add-fdip-icache-prefetch --strict` passes
- `srv67` smoke / 5M results exist
- `crypto14` and `compute_int_32` high-I$ sanity exists
- `fetch.icacheStallCycles` trace-mode witness is now live

Moderate:

- slice-level apply-clean evidence from bundle/application checks
- temporary-index staging preview:
  - `slice 1`: 8 files, `95 insertions(+), 3 deletions(-)`
  - `slice 2`: 8 files, `479 insertions(+), 5 deletions(-)`
  - `slice 2 + 3`: 12 files, `879 insertions(+), 14 deletions(-)`

Weaker:

- fresh isolated full builds for `slice 2` and `slice 2 + 3`
- fresh isolated full build for `slice 1`

## If we want stricter proof before staging

Run these three mechanical checks in fresh temp worktrees:

1. apply `01_slice1.patch` and build `gem5.opt` + `fetch_coverage.test`
2. apply `02_slice2.patch` and build `gem5.opt`
3. apply `02_slice2.patch` + `03_slice3.patch` and build `gem5.opt`

None of those are conceptually risky; they are just time-consuming.
