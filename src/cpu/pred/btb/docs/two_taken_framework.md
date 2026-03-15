# 2-Taken Framework

## Overview

The 2-Taken framework extends `DecoupledBPUWithBTB` so one prediction opportunity can produce:

- `block0`: the normal finalized prediction for the current fetch block
- `block1`: an optional next fetch block derived from the speculative post-state of `block0`

The fetch side is intentionally unchanged at the interface level. It still consumes ordinary single-block `FetchTarget` entries one by one. The framework only allows the predictor side to enqueue two consecutive targets earlier.

## Design Goal

This framework is designed for experiments, not for a fully generalized N-block predictor. The first implementation focuses on:

- keeping `FetchTarget` as the external unit of fetch, squash, and recovery
- generating at most two blocks per prediction
- allowing each predictor to decide independently whether it actively participates in `block1`
- allowing unsupported `block1` branch types to be filtered conservatively

## High-Level Architecture

### 1. External Model Stays Single-Block

The following structures remain single-block externally:

- `FetchTarget`
- `FetchTargetQueue`
- fetch-side consumption in `src/cpu/o3/fetch.cc`

The framework does not make fetch consume two targets in one cycle. Instead, the predictor may enqueue two targets in order.

### 2. Internal Bundle Model

`DecoupledBPUWithBTB` now has an internal bundle path:

- `SpecState`
  - `pc`
  - `history`
  - `phistory`
  - `bwhistory`
  - `lhistory`
- `PredictionBundle`
  - `pred0`
  - optional `pred1`
  - `stateAfter0`
  - `stateAfterFinal`
  - `pred1DropReason`

`pred1` is generated from `stateAfter0`, not from the original thread state.

### 3. Prediction Flow

The flow is:

1. Generate the normal final prediction for `block0`
2. Compute speculative next state after `block0`
3. Check top-level `block1` gates
4. If allowed, run predictor `block1` hooks using `stateAfter0`
5. Finalize `pred1`
6. Enqueue `target0`
7. If valid, enqueue `target1`
8. Commit thread speculative state once using `stateAfterFinal`

## Predictor Participation Model

Each `TimedBaseBTBPredictor` now has a `block1Participate` control.

Two modes are supported:

- `active`
  - the predictor runs its own `putPCHistoryForBlock1(...)`
- `pass-through`
  - the predictor does not actively predict `block1`
  - instead, it can preserve lower-stage information from `lowerPred`

### Current Implemented Behavior

- `UBTB`
  - active `block1` prediction path is implemented
- `TAGE`
  - active mode uses normal prediction path
  - passive mode copies `condTakens` and `tageInfoForMgscs` from `lowerPred`
- `ITTAGE`
  - passive mode copies `indirectTargets` from `lowerPred`
- `RAS`
  - passive mode copies `returnTarget` from `lowerPred`
- `MBTB`
  - passive mode copies `btbEntries` from `lowerPred`

## Top-Level Block1 Gating

Before `block1` is accepted, the bundle logic checks:

- `enableTwoTaken`
- `dropBlock1OnBlock0Override`
- `dropBlock1WhenFTQHasOnlyOneSlot`
- valid next PC after `block0`
- whether `block0` had an initial uBTB hit

Then the finalized `pred1` is filtered for unsupported branch classes:

- conditional branch without direction support
- indirect branch without indirect-target support
- return without RAS support

Support can come from either:

- active predictor participation, or
- copied support preserved from `lowerPred`

## Configuration Parameters

The framework adds the following top-level controls in `DecoupledBPUWithBTB`:

- `enableTwoTaken`
  - enable the two-block bundle path
- `dropBlock1OnBlock0Override`
  - drop `block1` when `block0` is overridden by a later predictor stage
- `dropBlock1WhenFTQHasOnlyOneSlot`
  - require at least two free FTQ slots before creating `block1`
- `dropBlock1OnCondWithoutTage`
  - reject `block1` conditional branches when no valid direction support exists
- `dropBlock1OnIndirectWithoutIttage`
  - reject `block1` indirect branches when no valid indirect-target support exists
- `dropBlock1OnReturnWithoutRas`
  - reject `block1` returns when no valid return target exists

Each predictor also inherits:

- `block1Participate`
  - whether that predictor actively predicts `block1`

## Files Touched by the Framework

Core logic:

- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/timed_base_pred.hh`
- `src/cpu/pred/btb/timed_base_pred.cc`

Configuration surface:

- `src/cpu/pred/BranchPredictor.py`

Predictor hooks:

- `src/cpu/pred/btb/btb_ubtb.hh`
- `src/cpu/pred/btb/btb_ubtb.cc`
- `src/cpu/pred/btb/btb_tage.hh`
- `src/cpu/pred/btb/btb_ittage.hh`
- `src/cpu/pred/btb/ras.hh`
- `src/cpu/pred/btb/mbtb.hh`

Queue behavior:

- `src/cpu/pred/btb/ftq.hh`
- `src/cpu/pred/btb/ftq.cc`

## Statistics

The framework adds first-stage block1 statistics in `DBPBTBStats`:

- `block1Attempted`
- `block1Accepted`
- `block1DroppedByBlock0Override`
- `block1DroppedByCond`
- `block1DroppedByIndirect`
- `block1DroppedByReturn`
- `block1DroppedByFTQFull`
- `block1DroppedOther`

These counters are intended to support ablation studies and help explain performance changes.

## Tests Added

Current focused test coverage includes:

- `src/cpu/pred/btb/test/btb.test.cc`
  - block1 drop-reason helpers
  - copied-support acceptance cases
  - `UBTB` block1 prediction behavior
  - `MBTB` block1 pass-through behavior
- `src/cpu/pred/btb/test/btb_tage.test.cc`
  - `TAGE` block1 active path
  - `TAGE` block1 passive copy path
- `src/cpu/pred/btb/test/fetch_target_queue.test.cc`
  - two-target squash behavior
  - FTQ free-slot accounting for two-target admission

## Current Scope and Limitations

This implementation intentionally does not yet do the following:

- fetch consuming two targets in one cycle
- N-block generalization beyond two blocks
- full block1 specialization for every predictor component
- broad end-to-end recovery validation for every history source

The framework is intended to be usable for current 2-Taken experiments while leaving room for later refinement.

## Suggested Experimental Starting Points

Useful first configurations include:

1. `UBTB` active, all other predictors passive
2. `UBTB + TAGE` active, `ITTAGE/RAS` passive
3. `UBTB + TAGE + ITTAGE + RAS` active
4. `dropBlock1OnBlock0Override = true` vs `false`
5. conservative filtering on vs off for conditional / indirect / return cases

These settings should give a good first view of the latency/accuracy tradeoff introduced by the two-block framework.
