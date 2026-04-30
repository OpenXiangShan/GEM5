# FDIP Parameters / Stats / Limitations (2026-04-22)

## Scope

This note records the **current implemented** FDIP surface on
`fdip-phase2-xsdev`.

It is intentionally Trellis-owned and implementation-grounded:

- only parameters that exist in code today are listed
- only stats that are emitted today are listed
- limitations describe the current model, not the ideal RTL-aligned future

## User-visible parameter surface

Current CLI wiring lives in:

- `configs/common/Options.py`
- `configs/common/xiangshan.py`
- `configs/example/kmhv3.py`
- `src/cpu/pred/BranchPredictor.py`
- `src/cpu/o3/BaseO3CPU.py`

### Parameters

1. `--enable-fdip`
   - default: `False`
   - meaning: enable FTQ-directed ICache prefetch in `DecoupledBPUWithBTB`

2. `--bpu-runahead-entries`
   - default: `8`
   - meaning: limit `next-alloc - fetchPtr` distance in FTQ entries
   - important: this limit only matters when FDIP is enabled
   - `0` disables the limit

3. `--fdip-lookahead-entries`
   - default: `1`
   - meaning: max `prefetchPtr - fetchPtr` distance in FTQ entries

4. `--fdip-issue-bandwidth`
   - default: `1`
   - meaning: max FDIP issue bandwidth in cachelines per cycle

5. `--fdip-max-outstanding`
   - default: `8`
   - meaning: max outstanding FDIP cacheline requests

6. `--prefetch-lines-per-ftq`
   - default: `cover_actual_fetch_range`
   - choices:
     - `start_line_only`
     - `cover_actual_fetch_range`
   - meaning:
     - `start_line_only`: only prefetch the start cacheline
     - `cover_actual_fetch_range`: cover the actual fetch request span

7. `--fdip-flush-partial-on-epoch-change`
   - default: `on`
   - meaning: flush per-entry FDIP partial state when redirect/epoch changes

8. `--no-fdip-flush-partial-on-epoch-change`
   - meaning: keep per-entry partial state across epoch change
   - current code restricts this path for FDIP-enabled use

9. `--fdip-drop-refill-on-epoch-mismatch`
   - default: `False`
   - meaning: drop old-path FDIP refill installation on epoch mismatch
   - practical note: the current tuned research-on configuration turns this on

10. `--fdip-recent-unused-cycles`
    - default: `0`
    - meaning: suppress FDIP issue for recently-unused lines for `N` cycles
    - `0` disables recent-unused suppression

### Additional config-only wiring

When `enable_fdip` is on in `configs/example/kmhv3.py`:

- `cpu.icache.mshrs = 14`
- `cpu.icache.demand_fetch_mshrs = 4`
- `cpu.icache.fdip_prefetch_mshrs = 10`
- `cpu.fdipIcacheAccessor = cpu.icache`

This is the current tuned-on path for direct probe / recent-unused checks.

## Recommended current research-on knobs

The current Trellis validation anchor uses:

- `--enable-fdip`
- `--fdip-lookahead-entries=1`
- `--fdip-issue-bandwidth=1`
- `--fdip-max-outstanding=2`
- `--bpu-runahead-entries=0`
- `--fdip-drop-refill-on-epoch-mismatch`
- `--fdip-recent-unused-cycles=65536`

## Key stats

### Fetch-side stats (`system.cpu.fetch.*`)

Defined in `src/cpu/o3/fetch.cc` / `fetch.hh`.

Most useful current stats:

- `fdipIssuedLines`
  - FDIP cacheline prefetches successfully sent
- `fdipDropped`
  - best-effort issue attempts dropped
- `fdipFilteredFault`
  - filtered by translation fault
- `fdipFilteredUncacheable`
  - filtered by uncacheable region
- `fdipFilteredRecentUnused`
  - filtered by recent-unused suppression
- `fdipCandidateLines`
  - candidate line touches derived from predictor targets
- `fdipUniqueCandidateLines`
- `fdipRepeatedCandidateLines`
- `fdipDirectProbeHit`
  - translated FDIP lines that hit via direct ICache probe
- `fdipUniqueIssuedLines`
- `fdipRepeatedIssuedLines`
- `fdipWrongPathIssuedLines`
- `fdipWrongPathUniqueIssuedLines`
- `fdipWrongPathDemandAccesses`
- `fdipWrongPathDemandReusedLines`
- `fdipWayHintsInstalled`
- `fdipWayHintsConsumed`
- `fdipOutstandingMax`
- `fdipEpochMismatch`
  - stale translation/response events ignored by fetch-side epoch tracking
- `icacheStallCycles`
  - now valid in trace mode after the fetch-side stall-accounting fix

### Cache-side stats (`system.cpu.icache.*`)

Defined in `src/mem/cache/base.cc` / `base.hh`.

Most useful current stats:

- `fdipInstalled`
  - installed L1I fills caused by FDIP-triggered misses
- `fdipUsefulHits`
  - demand hits on FDIP-prefetched L1I lines
- `fdipLate`
  - FDIP arrived too late, demand already needed the line
- `fdipUnused`
  - FDIP-prefetched L1I lines evicted unused
- `fdipEpochMismatch`
  - old-path FDIP refill epoch mismatches observed at cache side
- `fdipDroppedRefill`
  - old-path FDIP refills dropped from installation
- `fdipProbeHit`
  - direct tag probe hit avoids miss allocation
- `fdipProbeMerged`
  - merged onto existing in-flight miss
- `fdipMissAlloc`
  - FDIP misses that allocate real MSHRs
- `fdipMissAllocColdOrUnknown`
- `fdipMissAllocAfterUseful`
- `fdipMissAllocAfterUnused`
- `fdipMissAllocAfterUnusedThenLate`
- `fdipMissAllocAfterUnusedThenUseful`
- `fdipMissAllocAfterUnusedThenUnused`
- `fdipMissAllocAfterUnusedThenDropped`

### Practical metric sets

For quick P0 evaluation, the current most useful bundle is:

- `system.cpu.fetch.icacheStallCycles`
- `system.cpu.fetch.fdipIssuedLines`
- `system.cpu.fetch.fdipFilteredRecentUnused`
- `system.cpu.icache.fdipDroppedRefill`
- `system.cpu.icache.fdipUsefulHits`
- `system.cpu.icache.fdipLate`
- `system.cpu.icache.fdipUnused`
- `system.cpu.icache.fdipMissAllocAfterUnused`
- `system.cpu.icache.fdipMissAllocAfterUnusedThenUseful`
- `system.cpu.icache.fdipMissAllocAfterUnusedThenUnused`
- `system.cpu.icache.overallMisses::total`
- `system.cpu.icache.overallAvgMissLatency::total`
- `system.cpu.ipc`

## Current limitations

### Modeling limitations

1. This is still a Phase 1 / 1.5 style model, not full RTL alignment.
2. The current path does not yet expose a full tag/data split ICache interface.
3. `prefetchPtr` / FTQ peek plumbing is not fully implemented as described in
   the longer OpenSpec roadmap.
4. Contention explanation is still mostly stats-driven, not backed by a richer
   directed arbitration test suite.

### Verification limitations

1. `system.cpu.iew.fetchStallReason::IcacheStall` still remains zero in current
   trace-mode runs even though `system.cpu.fetch.icacheStallCycles` now moves.
2. The redirect cleanup proof is currently helper-level:
   - `src/cpu/o3/fdip_cleanup.hh`
   - `src/cpu/o3/fdip_cleanup.test.cc`
   It proves cleanup contract behavior, not a full mid-flight redirect harness.
3. Slice-level isolated full builds are still weaker than main-worktree build
   and integrated trace evidence.

### Usability limitations

1. `kmhv3.py` is the main tuned-on config path; other config entrypoints are
   not yet documented as first-class FDIP paths.
2. The current Trellis docs are the most accurate source of truth; OpenSpec
   checklist text lags behind the actual landed state.

## Recommended next decision

Current recommendation:

- treat this work as complete for the P0/P1 stabilization cut
- stop at Phase 1 / 1.5 unless there is a specific research need that cannot be
  answered with the current miss-latency / pollution / useful-late-unused data

Reasons:

1. The current tuned-on path is already validated on `srv67`, `crypto14`, and
   `compute_int_32`.
2. The major P0 questions have concrete answers now:
   - repeated bad lines dropped sharply
   - old-path refill pollution is observable and suppressible
   - `fetch.icacheStallCycles` now moves in the right direction on high-I$ traces
3. The remaining work is increasingly about model fidelity and proof quality,
   not about basic P0 viability.
