# DecoupledBPUWithBTB

`DecoupledBPUWithBTB` produces staged branch predictions, accounts for override
bubbles, and enqueues FetchTargets for instruction fetch.

## Data Model

| Type | Responsibility |
|---|---|
| `BranchAttribute` / `BranchInfo` | Shared classification, PC, destination and instruction size |
| `BTBEntry` | Component storage, including counter, tag and validity |
| `BranchPrediction` | Candidate direction, final target, base direction/strength and source |
| `FullBTBPrediction` | One stage's ordered candidates and TAGE information for MGSC |
| `BranchOutcome` | Actual execution facts, reusing the common branch description |
| `FetchTarget` | Prediction/history snapshots, component metadata and execution facts |

`FullBTBPrediction::btbEntries` contains `BranchPrediction` objects ordered by PC.
Each candidate carries `taken` and `predictedTarget`; there are no separate
conditional-direction, indirect-target or RAS-target maps. Inherited `target`
remains the original BTB destination for existing training and fallback users.

Candidates retain the not-taken conditional prefix up to the first taken branch.
Direction history consumes that prefix; the selected next PC and path history
use the final predicted target. The saved base direction is independent of later
TAGE/MGSC overrides. Components retain native storage snapshots and provider
metadata for training; a candidate does not carry a BTB counter or tag.

## Stage Composition

`requestNewPrediction(tid)` clears stage results, assigns their thread, ASID hash
and block PC, and calls the enabled components' `putPCHistory()` in this order:

```text
uBTB -> aBTB -> MicroTAGE -> PairTAGE -> MBTB -> TAGE -> RAS -> ITTAGE -> MGSC
```

Components still fill stages from their configured `numDelay` onward. TAGE,
MicroTAGE and MGSC update candidate directions; RAS and ITTAGE update final
targets. Each stage retains its own candidate set, and repeated-PC metadata
retains the existing later-stage overwrite order. This data-model refactor does
not introduce a new pipeline or move composition into a new top-level API.

PairTAGE keeps its private storage, First/Second phase machine, prediction and
training live entries, confidence and squash behavior. Its output adapter clears
older candidates on a hit: a taken block emits one candidate, while a fallthrough
block leaves the vector empty. A miss preserves earlier predictions.

`generateFinalPredAndCreateBubbles()` retains the latest nonempty-stage selection,
S3 fast-training and override comparison rules. `processNewPrediction()` waits
for override bubbles and FTQ space before enqueuing a FetchTarget and updating
speculative histories. The optional PairTAGE second block uses the same metadata,
FTQ-capacity and history-update paths as before.

## Training and Recovery

FetchTargets retain component metadata through `getPredictionMeta(tid)`.
`PredictionUpdateContext` and `PreparedUpdate` combine the saved prediction state
with execution outcomes for the existing resolve/commit update paths.
`HistoryRecoveryContext` supplies the snapshot used during squash recovery.

PairTAGE concurrent SMT prediction remains unsupported; enabling it with more
than one predicting thread per cycle is rejected. Production uRAS is not part of
this BPU's enabled component list; the `uras.test` target tests a mock stack.
