# Full Resolve Train Review Guide

## 1. Why this branch exists

This branch changes GEM5 frontend resolved-branch training from a squash-assisted,
PC-only update model to an RTL-aligned full resolve-train model.

Before this branch:

- IEW only sent `{ftqId, pc}` through `resolvedCFIs`
- Fetch reconstructed resolved updates from `FetchTarget` state
- correct training truth depended on squash writing back `exeTaken` /
  `exeBranchInfo` before resolve update was consumed

After this branch:

- IEW sends full per-branch resolve truth
- Fetch aggregates real resolved branches by FTQ target identity
- Fetch builds an explicit `ResolvedTrainPacket`
- migrated predictors train from packet truth plus prediction-time metadata

The main motivation is to remove the correctness and performance risk caused by
resolve training depending on squash timing.

## 2. High-level architecture

The new dataflow is:

```text
IEW
  -> resolveTrainEntries[{ftqId, generation, pc, target, taken, ...}]
Fetch
  -> resolveTrainQueue keyed by {tid, ftqId, generation}
  -> ResolvedTrainPacket{startPC, predMetas, realBranches}
DecoupledBPUWithBTB
  -> resolveTrain(packet)
  -> per-component canResolveTrain/resolveTrain
```

The old path still exists as fallback:

```text
IEW
  -> resolvedCFIs[{ftqId, pc}]
Fetch
  -> legacy resolveQueue
DecoupledBPUWithBTB
  -> prepareResolveUpdateEntries/markCFIResolved/resolveUpdate
```

Current default mode is:

- `enableFullResolveTrain = True`
- `enableLegacyResolveUpdate = True`

This means:

- migrated components use the full packet path
- non-migrated `resolvedUpdate` components still keep legacy fallback

It is not a double-update mode for migrated components.

## 3. Review map by file group

### 3.1 O3 protocol and Fetch plumbing

Relevant files:

- `src/cpu/o3/BaseO3CPU.py`
- `src/cpu/o3/comm.hh`
- `src/cpu/o3/dyn_inst.hh`
- `src/cpu/o3/dyn_inst.cc`
- `src/cpu/o3/iew.cc`
- `src/cpu/o3/fetch.hh`
- `src/cpu/o3/fetch.cc`
- `src/cpu/pred/BranchPredictor.py`

Key changes:

- adds rollout params: `enableFullResolveTrain`, `enableLegacyResolveUpdate`
- adds `ResolveTrainEntry` to `IewComm`
- records `ftqGeneration` and `ftqOffset` in `DynInst`
- emits full resolve truth from IEW
- adds `resolveTrainQueue` in Fetch
- builds `ResolvedTrainPacket` from queued truth plus `FetchTarget` metadata
- only pops packet queue on explicit predictor acceptance

Main review questions:

- does `DynInst` carry enough fetch-time identity for stable FTQ matching?
- does Fetch aggregate by `{tid, ftqId, generation}` correctly?
- are stale, squashed, committed, and reused FTQ targets rejected safely?
- does full-resolve retry feed the same throttle path as legacy resolve update?

### 3.2 FTQ identity protection and predictor top-level API

Relevant files:

- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/ftq.hh`
- `src/cpu/pred/btb/ftq.cc`
- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/timed_base_pred.hh`

Key changes:

- adds `generation` to `FetchTarget`
- allocates a fresh generation when a logical FTQ target is created
- adds FTQ identity helpers: generation lookup and identity matching
- adds packet types:
  - `FetchTargetIdentity`
  - `ResolvedBranch`
  - `ResolvedTrainPacket`
- adds `DecoupledBPUWithBTB::resolveTrain()`
- adds default component hooks:
  - `canResolveTrain(packet)`
  - `resolveTrain(packet)`
- adds top-level packet validation before fan-out

Main review questions:

- is FTQ generation sufficient to reject stale resolve traffic?
- does packet validation reject malformed branch lists and stale metadata?
- does `resolveTrain()` preserve the old probe/apply contract semantics?

### 3.3 Migrated predictors

Relevant files:

- `src/cpu/pred/btb/mbtb.hh`
- `src/cpu/pred/btb/mbtb.cc`
- `src/cpu/pred/btb/btb_tage.hh`
- `src/cpu/pred/btb/btb_tage.cc`
- `src/cpu/pred/btb/btb_ittage.hh`
- `src/cpu/pred/btb/btb_ittage.cc`

Migrated components:

- `MBTB`
- `BTBTAGE`
- `BTBITTAGE`

Current split:

- in full-resolve mode, these three no longer consume legacy resolved-update
- other non-migrated components can still use legacy resolved-update if enabled

Main review questions:

- does each component now train only from packet truth on the new path?
- are legacy side effects cleanly disabled for migrated components?
- do bank conflict / readiness semantics still behave the same?

## 4. Component-by-component summary

### 4.1 MBTB

What changed:

- packet-based `canResolveTrain()` / `resolveTrain()` were added
- new path no longer depends on:
  - `updateBTBEntries`
  - `updateNewBTBEntry`
  - per-entry resolved bits inside `FetchTarget`
- packet updates reuse existing SRAM / victim-cache update machinery

Important review points:

- MBTB legacy resolved-update is skipped in full-resolve mode
- MBTB-specific legacy prepare/mark side effects are also gated off in
  full-resolve mode

### 4.2 BTBTAGE

What changed:

- packet-based bank-conflict probe added in `canResolveTrain()`
- packet path now trains using prediction snapshots, not squash-populated state
- metadata now retains predicted conditional `BTBEntry` per branch PC
- missing-meta conditional branches on the packet path are now materialized and
  still trained, matching old new-entry behavior

Important review points:

- packet path only trains intended conditional branches
- packet conflict failures now drive `notifyResolveFailure()` so retry and
  prediction throttling still work

### 4.3 BTBITTAGE

What changed:

- packet-based `resolveTrain()` now uses indirect branch truth from packet data
- no longer depends on squash-derived `exeBranchInfo` on the new path
- legacy resolved-update is skipped in full-resolve mode

Important review points:

- training remains scoped to indirect non-return branches
- alternate-provider update now only happens when `alt_info.found` is true,
  fixing an existing corruption hazard in both old and new paths

## 5. Current default behavior

Current defaults are set in `src/cpu/o3/BaseO3CPU.py`:

- `enableFullResolveTrain = True`
- `enableLegacyResolveUpdate = True`

This is intentional.

Reason:

- migrated components already use the packet path
- some other `resolvedUpdate` users may still exist outside this migration set
- keeping legacy enabled avoids silent loss of resolve-stage training during the
  rollout period

So current behavior is:

- `MBTB`, `BTBTAGE`, `BTBITTAGE` -> full packet path
- non-migrated resolved-update components -> legacy path
- commit-time predictors -> unchanged commit path

## 6. Verification done on this branch

Fresh verification used before final commit creation:

- build: `scons build/RISCV/gem5.opt --gold-linker -j60`
- unit test: `build/RISCV/cpu/pred/btb/test/tage.test.debug`

Observed result:

- `gem5.opt` builds successfully
- `tage.test.debug` passes `21/21`

New or extended test coverage includes packet-mode BTBTAGE cases for:

- bank-conflict probe behavior
- packet-truth conditional selection
- new conditional entry training without prediction metadata

## 7. Suggested review order

For fastest review, read in this order:

1. `src/cpu/o3/comm.hh`
2. `src/cpu/o3/iew.cc`
3. `src/cpu/o3/fetch.hh`
4. `src/cpu/o3/fetch.cc`
5. `src/cpu/pred/btb/common.hh`
6. `src/cpu/pred/btb/ftq.hh`
7. `src/cpu/pred/btb/decoupled_bpred.hh`
8. `src/cpu/pred/btb/decoupled_bpred.cc`
9. `src/cpu/pred/btb/mbtb.cc`
10. `src/cpu/pred/btb/btb_tage.cc`
11. `src/cpu/pred/btb/btb_ittage.cc`
12. `src/cpu/pred/btb/test/btb_tage.test.cc`

## 8. Known follow-up work

This branch does not yet remove the legacy path.

Natural next steps after review:

- migrate any remaining `resolvedUpdate` components if needed
- once all needed components are packetized, turn `enableLegacyResolveUpdate`
  default off
- then delete legacy `resolvedCFIs` / `prepareResolveUpdateEntries()` /
  `markCFIResolved()`-based training path
