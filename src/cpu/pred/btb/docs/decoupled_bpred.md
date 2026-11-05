# DecoupledBPUWithBTB

`DecoupledBPUWithBTB` composes BTB candidates, direction predictions and target
predictions, then enqueues FetchTargets for instruction fetch.

## Data Model

| Type | Responsibility |
|---|---|
| `BranchAttribute` / `BranchInfo` | Branch classification, PC, destination and instruction size |
| `BTBEntry` | Component storage, including counter, tag and validity |
| `BranchPrediction` | Stage candidate: validity, direction, final target, base direction/strength and source |
| `FullBTBPrediction` | One stage's candidates, hit state and TAGE information for MGSC |
| `BranchOutcome` | Actual execution facts, reusing `BranchInfo` |
| `FetchTarget` | Prediction/history snapshots, component metadata and observed execution facts |

`FullBTBPrediction::btbEntries` is a vector of `BranchPrediction`, ordered by PC.
Each candidate holds its direction in `taken` and its selected destination in
`predictedTarget`; there are no separate conditional-direction, indirect-target
or RAS-target maps. Inherited `target` remains the raw BTB destination for
existing training and fallback consumers.

Keep the not-taken conditional prefix: direction history includes these branches
up to the first taken conditional or unconditional branch. Path history and the
FetchTarget's selected next PC use the final predicted target.

## Composition

`requestNewPrediction(tid)` resets stage results and assigns their thread, ASID
hash and block PC. `predictComponents(tid)` visits enabled components in order:

```text
uBTB -> aBTB -> MicroTAGE -> PairTAGE -> MBTB -> TAGE -> RAS -> ITTAGE -> MGSC
```

- BTBs return candidates and retain native storage snapshots for training.
  A native aBTB hit owns its candidate set; an aBTB miss may reuse uBTB.
- MicroTAGE processes aBTB candidates. TAGE and MGSC return direction validity
  and direction; the BPU applies base or TAGE fallback when appropriate.
- RAS supplies return targets. ITTAGE supplies indirect-target validity and
  targets, with raw BTB targets as fallback.
- PairTAGE supplies a block and maintains its own prediction metadata. Its
  second-block path uses a side-effect-free MBTB/TAGE teacher lookup.

Each component applies from its configured `numDelay` onward. Direction and
indirect-target lookup run separately for each applicable stage's candidate set;
metadata for a repeated PC retains the existing later-stage overwrite order.
This is an explicit composition model, not a new cycle-by-cycle RTL pipeline.

`generateFinalPredAndCreateBubbles()` keeps the existing latest-nonempty-candidate
selection rule, compares earlier predictions, performs S3 fast training and
records override bubbles. `hit` can represent branchless fallthrough, but does
not replace that final selection rule. `processNewPrediction()` consumes the
bubbles before enqueuing the FetchTarget and updating speculative histories.

## Training and Recovery

FetchTargets retain component metadata through `getPredictionMeta(tid)`.
`PredictionUpdateContext` and `PreparedUpdate` provide saved prediction state
and actual branch outcomes to the existing resolve/commit update paths.
`HistoryRecoveryContext` supplies the snapshot used during squash recovery.

Ordinary components keep thread-specific prediction/history state. PairTAGE
concurrent SMT prediction remains unsupported: the constructor rejects enabled
PairTAGE when more than one thread predicts per cycle. Production uRAS is not
part of this BPU's enabled component list.
