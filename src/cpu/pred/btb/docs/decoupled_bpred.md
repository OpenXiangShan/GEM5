# DecoupledBPUWithBTB

`DecoupledBPUWithBTB` produces staged branch predictions, accounts for override
bubbles, and enqueues FetchTargets for instruction fetch.

## Data Model

| Type | Responsibility |
|---|---|
| `BranchInfo` | Shared classification, PC, destination and instruction size |
| `BTBEntry` | Component storage, including counter, tag and validity |
| `BranchPrediction` | Candidate direction, final target, base direction/strength and source |
| `FullBTBPrediction` | One stage's ordered candidates and TAGE information for MGSC |
| `BranchOutcome` | Actual execution facts, reusing the common branch description |
| `FetchTarget` | Prediction/history snapshots, component metadata and execution facts |

`FullBTBPrediction::btbEntries` contains `BranchPrediction` objects ordered by PC.
The storage, prediction and execution records each contain a `BranchInfo branch`
value; they do not inherit fields or comparison operators. `branch.pc` comparisons
are explicit at identity lookups. Each candidate carries `taken` and a single
`branch.target`. BTB lookup initializes the target; ITTAGE or RAS refines it in
place. ITTAGE falls back to its input
candidate target on a miss. S3 fast-training of uBTB, AheadBTB and PairTAGE
consumes the final candidate target, matching RTL fastTrain. Component-local
BTB storage and prediction metadata retain their own snapshots.

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

`twoTakenFilterIndirectCallsAndReturns` defaults to `True`. A second block
whose predicted exit is an indirect call or return follows the normal second
phase, where ITTAGE or RAS can refine its target. `secondBlockTypeFiltered`
counts these blocks after the FTQ run-ahead and capacity checks.

## PairTAGE Statistics

Under `system.cpu.branchPred`, `pairtageFirstBlockEnqueued` counts every FTQ
entry enqueued in PairTAGE's first phase, even on a lookup miss. Divide
`secondBlockEnqueued` by this count for the fraction of pair starts that
enqueue both blocks in one cycle. `secondBlockFtqBlocked` counts available
second blocks that cannot be enqueued in that cycle due to the FTQ run-ahead
limit or capacity; it replaces `secondBlockRunAhead` and `secondBlockFtqFull`.

`pairtageBlock{Enqueued,Evaluated,Correct}ByType_<role>::<type>` bins blocks
by predicted exit type at enqueue. The roles are `two_taken_first`,
`two_taken_second`, and `ordinary_second` (the normal prediction in PairTAGE's
second phase, even if another predictor supplies the final result). The exit
types are `cond_branch`, `direct_call`, `indirect_call`, `return`, `direct_jump`,
`indirect_jump`, `unknown`, and `fallthrough`. A `::total` sums each role.

- Divide `Enqueued` for a type by that role's `Enqueued::total` for the type
  share. This measures the speculative output, including wrong-path entries.
- Divide `Correct` by `Evaluated` for the same role and type (or `::total`).
  Commit evaluates the original FTQ prediction against the first retired
  taken branch's PC and target, or whether execution reached the predicted
  fall-through boundary. Blocks discarded on an older redirect, and blocks
  interrupted before their exit can be checked, do not enter the denominator.

The raw counters can be extracted with
`.agents/skills/frontend-pmu-analysis/configs/pairtage_counters.txt`.

## Training and Recovery

FetchTargets retain component metadata through `getPredictionMeta(tid)`.
`PredictionUpdateContext` and `PreparedUpdate` combine the saved prediction state
with execution outcomes for the existing resolve/commit update paths.
`HistoryRecoveryContext` supplies the snapshot used during squash recovery.

PairTAGE concurrent SMT prediction remains unsupported; enabling it with more
than one predicting thread per cycle is rejected. Production uRAS is not part of
this BPU's enabled component list; the `uras.test` target tests a mock stack.
