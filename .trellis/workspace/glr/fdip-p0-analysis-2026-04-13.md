# FDIP P0 Analysis Note (2026-04-13)

## Scope

This note captures the current P0 validation status for
`add-fdip-icache-prefetch` on worktree `fdip-phase2-xsdev`.

Current tuned research-on configuration:

- `--enable-fdip`
- `--fdip-lookahead-entries=1`
- `--fdip-issue-bandwidth=1`
- `--fdip-max-outstanding=2`
- `--bpu-runahead-entries=0`
- `--fdip-drop-refill-on-epoch-mismatch`
- `--fdip-recent-unused-cycles=65536`

## Main outcomes

### `srv67` smoke (500k / warmup 100k)

- `fdipMissAllocAfterUnusedThenUnused`: `3258 -> 46` (`-98.59%`)
- `fdipMissAllocAfterUnusedThenUseful`: `240 -> 17` (`-92.92%`)
- `fdipDroppedRefill`: `0 -> 7420`
- `fdipFilteredRecentUnused`: `0 -> 734`
- `IPC`: `1.530011 -> 1.529824` (`-0.01%`)

Interpretation:

- the two intended P0 mechanisms both fire;
- repeated bad FDIP lines collapse sharply;
- smoke-level performance is essentially neutral.

### `srv67` 5M (warmup 1M)

- `fdipMissAllocAfterUnusedThenUnused`: `57111 -> 2116` (`-96.29%`)
- `fdipMissAllocAfterUnusedThenUseful`: `1525 -> 303` (`-80.13%`)
- `fdipDroppedRefill`: `0 -> 71580`
- `fdipFilteredRecentUnused`: `0 -> 5084`
- `fdipUsefulHits`: `6409 -> 1386`
- `fdipUnused`: `61714 -> 3025`
- `IPC`: `1.817250 -> 1.824324` (`+0.39%`)
- `overallAvgMissLatency::total`: `517.72 -> 310.22` (`-40.08%`)

Interpretation:

- benefit is still dominated by miss-start timeliness / pollution reduction,
  not by preserving historical useful-hit volume;
- old-path refill drop is very active and likely doing real work;
- the current tuned-on path remains slightly performance-positive on this
  anchor.

## High-I$ trace sanity

### `crypto14` (500k / warmup 100k)

FDIP-off -> FDIP-on:

- `fetch.icacheStallCycles`: `35513 -> 27065` (`-23.79%`)
- `fdipIssuedLines`: `0 -> 3920`
- `fdipFilteredRecentUnused`: `0 -> 101`
- `fdipDroppedRefill`: `0 -> 1636`
- `fdipUsefulHits`: `0 -> 206`
- `fdipLate`: `0 -> 402`
- `fdipUnused`: `0 -> 304`
- `overallAvgMissLatency::total`: `73073.06 -> 5874.71`
- `IPC`: `1.415438 -> 1.440066` (`+1.74%`)

Interpretation:

- this is a valid high-I$-pressure sanity trace for the current P0 setup;
- timeliness stats are non-trivial and internally consistent;
- P0 suppression/drop logic does not just reduce traffic, it still leaves
  meaningful useful-hit activity.

### `compute_int_32` (500k / warmup 100k)

FDIP-off -> FDIP-on:

- `fetch.icacheStallCycles`: `12778 -> 6187` (`-51.58%`)
- `fdipIssuedLines`: `0 -> 1173`
- `fdipFilteredRecentUnused`: `0 -> 32`
- `fdipDroppedRefill`: `0 -> 699`
- `fdipUsefulHits`: `0 -> 145`
- `fdipLate`: `0 -> 145`
- `fdipUnused`: `0 -> 75`
- `overallAvgMissLatency::total`: `71492.75 -> 4966.56`
- `IPC`: `2.002754 -> 2.066248` (`+3.17%`)

Interpretation:

- this is a valid second high-I$ sanity trace for the current tuned-on path;
- the timeliness stats are smaller than `crypto14` but still clearly
  non-trivial and performance moves in the right direction.

### Rejected candidate: `compute_int_0` (500k / warmup 100k)

- `fdipIssuedLines`: `0 -> 31`
- `fdipDroppedRefill`: `0 -> 0`
- `fdipUsefulHits`: `0 -> 0`
- `fdipLate`: `0 -> 0`
- `fdipUnused`: `0 -> 0`
- `IPC`: `4.860432 -> 4.840552` (`-0.41%`)

Interpretation:

- under this short trace window, `compute_int_0` is not exercising enough
  I$ pressure to be a useful FDIP sanity trace.

## Validation-side findings

- `MicroTAGE` previously shadowed inherited `TimedBaseBTBPredictor`
  parameters (`blockSize`, `numDelay`), producing
  `MicroTAGE: branch ... blockSize=0` warnings.
- The fix changed those overrides to plain assignment and removed the warning.
- Smoke statistics before/after the fix remained bit-identical, so the fix
  improved validation trustworthiness without changing current FDIP outcomes.

## Stall-stat fix

- `fetch.icacheStallCycles` was previously unobservable in trace-mode because
  `profileStall()` was not wired into the active stall-update path, and
  trace-mode logical `StallReason::IcacheStall` did not flow through the same
  request-status check as demand ICache waits.
- The current worktree now:
  - calls `profileStall()` when fetch emits a zero-instruction stalled cycle,
  - falls back to `stallReason == IcacheStall` when no cache request state is
    available.
- After that fix, `fetch.icacheStallCycles` becomes usable on trace-mode runs:
  - `compute_int_32`: `12778 -> 6187`
  - `crypto14`: `35513 -> 27065`
- `system.cpu.iew.fetchStallReason::IcacheStall` still stays at `0` in the
  current trace-mode runs, so the fetch-side stat is the authoritative witness
  for this task.

## Cache-contract fix

- FDIP recent-unused / miss-alloc lifecycle tracking now keys by
  `(blkAddr, isSecure)` instead of raw `Addr`.
- This keeps suppression and prior-outcome accounting aligned with the
  externally visible `shouldSuppressFdipLine(addr, is_secure, ...)` contract
  and avoids cross-domain contamination if secure/non-secure lines share the
  same physical block address.

## Recommended next step

- Keep `srv67`, `crypto14`, and `compute_int_32` as the current P0 validation
  anchors.
- If we want to close the remaining stats inconsistency, investigate why
  `system.cpu.iew.fetchStallReason::IcacheStall` remains zero while
  `system.cpu.fetch.icacheStallCycles` now moves as expected.
