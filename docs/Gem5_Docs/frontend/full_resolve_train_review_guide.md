# Full Resolve Train Review Guide

## 1. Why this branch exists

This branch replaces the old squash-assisted resolved-update path with a single
packet-based full resolve-train path for the migrated BTB predictors.

Historically GEM5 used:

- `IEW -> resolvedCFIs`
- `Fetch.resolveQueue`
- `prepareResolveUpdateEntries()`
- `markCFIResolved()`
- `resolveUpdate()`

That flow depended on squash-populated execution truth and mixed together:

- resolve notification
- new-entry discovery
- predictor-specific update preparation

The new branch instead uses explicit resolve truth from IEW and packet-based
training.

## 2. Current architecture

### 2.1 Resolved-stage training path

Current resolved-stage dataflow is:

```text
IEW
  -> resolveTrainEntries[{ftqId, generation, pc, target, taken, ...}]
Fetch
  -> resolveTrainQueue keyed by FTQ target identity
  -> ResolvedTrainPacket{tid, target, startPC, realBranches}
DecoupledBPUWithBTB
  -> resolveTrain(packet)
  -> MBTB / BTBTAGE / BTBITTAGE
```

Important current semantics:

- packets are truth-only; they do not own frozen predictor metadata
- training uses live FTQ metadata from the current `FetchTarget`
- fetch-side packet formation trims branches after the first taken branch,
  matching RTL-style training-prefix semantics

### 2.2 Legacy path status

The old legacy resolved-update chain has been removed from the active training
architecture:

- no `resolvedCFIs` predictor training path
- no `Fetch.resolveQueue`-based predictor training path
- no predictor-top `prepareResolveUpdateEntries()` / `markCFIResolved()` /
  `resolveUpdate()` helper chain

Resolved-stage BTB training is now single-path: full resolve train.

### 2.3 Commit/update path

Commit/update behavior remains intact.

This matters because not every predictor needs resolved-stage packet training.
Current design is:

- migrated BTB predictors use full resolve-train
- other components may still rely on commit/update behavior

## 3. Component map

### 3.1 Components on full resolve-train

- `MBTB`
- `BTBTAGE`
- `BTBITTAGE`

### 3.2 Components not migrated to full resolve-train in this branch

These are not treated as active users of the removed legacy BTB resolved-update
chain in current configs:

- `MicroTAGE` (still has local old-style logic in code, but not enabled as an
  active resolved-stage user in current configs)
- `SC` remains follow-up work if full RTL parity is desired

### 3.3 Non-resolved-stage structures

These are not intended to use full resolve-train in the RTL-aligned model:

- commit/update-only or redirect/recover structures
- components whose role is speculative / fast-train / commit-time only

## 4. File map

### O3 / frontend integration

- `src/cpu/o3/comm.hh`
- `src/cpu/o3/dyn_inst.hh`
- `src/cpu/o3/dyn_inst.cc`
- `src/cpu/o3/iew.cc`
- `src/cpu/o3/fetch.hh`
- `src/cpu/o3/fetch.cc`
- `src/cpu/o3/BaseO3CPU.py`

These files now:

- carry full resolve truth from IEW
- maintain `resolveTrainQueue`
- build truth-only packets
- trim branches after the first taken branch before training

### Predictor top / FTQ integration

- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/ftq.hh`
- `src/cpu/pred/btb/ftq.cc`
- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/decoupled_bpred_stats.cc`
- `src/cpu/pred/btb/timed_base_pred.hh`
- `src/cpu/pred/btb/timed_base_pred.cc`

These files now:

- track FTQ target generation identity
- validate full resolve packets using structural checks
- dispatch full resolve packets to migrated components
- expose only the retained top-level resolve-train counters:
  `fullResolveTrainAccepted`, `fullResolveTrainRejectTargetMismatch`,
  `fullResolveTrainRejectPacketValidation`, and
  `fullResolveTrainRejectComponent`
- preserve commit/update path for non-resolved-stage training

### Migrated predictor components

- `src/cpu/pred/btb/mbtb.cc`
- `src/cpu/pred/btb/btb_tage.cc`
- `src/cpu/pred/btb/btb_ittage.cc`

## 5. Key behavior changes since early migration commits

The branch has moved beyond the earlier intermediate state described in older
notes.

Current important fixes include:

### 5.1 Truth-only packets

`ResolvedTrainPacket` no longer stores duplicated predictor metadata. Training
reads metadata from the live `FetchTarget` instead.

### 5.2 BTBTAGE new-entry handling

Full resolve-train now distinguishes:

- existing predicted entries
- new-entry candidates

so short-pattern conditional branches can allocate and grow similarly to the
legacy `update()` path without depending on squash-derived helper state.

### 5.3 RTL-style prefix trimming

When a packet contains multiple resolved branches, fetch trims the branch list to
the prefix up to and including the first taken branch.

This avoids the previous failure mode where packet validation rejected the whole
packet because branches existed after a taken branch.

## 6. Configuration semantics

Current configs are centered on `enableFullResolveTrain`.

The old `resolvedUpdate` concept used to mean:

- this component trains at resolve stage rather than only at commit

That intent is now expressed through the new full resolve-train path for the
migrated BTB predictors, not through a legacy helper chain.

### Current intent

- `kmhv3`: full resolve-train enabled for the migrated BTB set
- `idealkmhv3`: explicit control is still available through the top-level switch

## 7. What reviewers should focus on

### For architecture review

- Is the resolved-stage path now single-source and coherent?
- Is FTQ generation sufficient for stale-target filtering?
- Does fetch-side branch trimming match intended RTL behavior?

### For predictor review

- Do `MBTB`, `BTBTAGE`, and `BTBITTAGE` consume truth-only packets correctly?
- Is new-entry handling independent from squash-era legacy helper state?
- Does commit/update behavior remain intact where it should?

### For cleanup review

- Was the old `resolvedCFIs -> resolveQueue -> resolveUpdate()` chain removed
  cleanly?
- Did config semantics stop pointing at deleted legacy machinery?

## 8. Verification currently used on this branch

### Build

- `scons build/RISCV/gem5.opt --gold-linker -j60`

### Unit tests

- build: `scons build/RISCV/cpu/pred/btb/test/tage.test.debug --unit-test -j60`
- run: `build/RISCV/cpu/pred/btb/test/tage.test.debug --gtest_filter=BTBTAGETest.*`

Current retained coverage:

- the existing `BTBTAGETest.*` suite remains in place
- within the branch-added cleanup surface, the retained regressions are:
  `NewConditionalEntryWithoutPredictionMetaStillTrains`,
  `ResolveTrainBankConflict`,
  `ResolveTrainUsesPacketTruthForConditionalSelection`, and
  `ResolveTrainRepeatedShortPatternMatchesLegacyProviderGrowth`

Removed from the branch verification surface:

- rollout-time debug counters
- legacy resolve-update-only `BankConflict`
- exploratory `BTBTAGEUpperBound*` checks added during branch development

### Targeted workloads

Used repeatedly during this branch:

- `tage1`
- `usefulbit`
- `tage2`

The current branch state keeps these in the recovered performance range after the
packet-trimming fix and legacy-path cleanup.

## 9. Remaining follow-up work

Likely next steps after this branch stabilizes:

- decide whether `SC` should be migrated to full resolve-train
- decide whether `MicroTAGE` should be migrated to full resolve-train for closer
  RTL parity
- continue reducing stale-drop rate if workload-level gaps remain
