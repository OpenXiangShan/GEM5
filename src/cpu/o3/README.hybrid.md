# Hybrid CROB validation

The model keeps per-DynInst execution and architectural retirement, with a
persistent physical-entry deque alongside the flat ROB instruction list.
Each entry records former/latter remaining-member counts and runtime type
(`NORMAL`, `CC`, `CS`, `SC`); each DynInst carries an entry id and slot bit.
The temporary planner retains `NORMAL-S/C/N` distinctions for classification
and allocation statistics.

`rob_hybrid.hh` defines the planner and entry/slot state types.
`rob_hybrid.cc` implements Hybrid classification, allocation, member
accounting, configuration checks, and redirects. `rob.cc` owns the shared
instruction list, recovery walker, and policy dispatch.

`commitWidth` limits physical entries, including squashed-head draining.
Members retire in order through the existing architectural commit path. A
fault, strictly ordered operation, or squash-after may stop within an entry;
the entry stays allocated until its remaining members retire or are squashed.
There is no separate DynInst retirement quota. Non-Hybrid policies retain
their existing expanded group-window behavior.

Use `configs/example/kmhv3.py` with the following parameter (preserve the
inner quotes because `--param` evaluates a Python expression):

```sh
--param="system.cpu[0].RobCompressPolicy='hybrid'"
```

The configuration supplies one thread and disables value prediction by
default. Hybrid supports the existing load value predictors; Load fusion
remains disabled.
Constant folding and MOVI elimination are disabled; register move elimination
remains enabled. The group length is 8, rename width is 8, and physical commit
width is 8. Existing non-Load fusion remains active. Other `--param` assignments can
override group size and entry width afterward. `numROBEntries` is physical
capacity (352 in this configuration).

Add `--enable-value-prediction` to create a `CompositeValuePredictor` with
`IdealConstantLVP` at `predictors[0]` and `EStride` at `predictors[1]`.
Both predictors offer candidates; `CVPConfidenceArb(counterBits=6)` selects
one offered value when a candidate is available. Confidence ties favor
`predictors[0]`; when neither child offers a prediction, the load uses its
normal execution path. The `--param` interface can tune each
child's scalar parameters once it exists; it cannot assign a new SimObject
to `valuePred`.

```sh
--enable-value-prediction \
--param='system.cpu[0].valuePred.predictors[0].satCounterBits=1' \
--param='system.cpu[0].valuePred.predictors[1].logMaxConfidence=1' \
--param='system.cpu[0].valuePred.predictors[1].thresholdPercent=0.0'
```

Value prediction applies to eligible loads with one destination register.
These loads occupy a single C slot, or a standalone N entry when excluded
from compression. When recovering by squash, a value misprediction preserves
the load's actual result and removes every younger instruction, matching the
existing slot recovery boundary. Selective cancellation can retain unaffected
younger instructions; issued dependents require a fallback to squash.
Prediction checks and training remain per DynInst. Extending value
prediction to Simple instructions would require reviewing recovery boundaries
inside shared S slots.

Planning is O(renameWidth); insertion and removal accounting is O(1) per
member. Commit scans one bounded entry when selecting it, and handles at most
`commitWidth * CROB_instPerGroup` members per cycle. Redirect setup scans at
most the bounded ROB to find the retained prefix; the recovery walker retains
its existing configured per-cycle width. Full invariant scans only run with
ROB debug tracing enabled.

## Slot-aware recovery

| Redirect instruction | Include redirect instruction | Surviving members in target entry |
|---|---|---|
| any member | yes | older members only |
| any member | no | target and older members |

The target instruction's sequence number defines the boundary within the
target entry.
Members older than the target instruction in the same entry are retained,
while the target is retained only when the redirect excludes itself. All
younger entries are removed. Removing the last latter member downgrades the
target to `NORMAL` only if a nonempty former survives; its id is preserved
and it is never paired with a new allocation. Full trap/TC flushes and
already-retired squash-after anchors retain the architectural sequence
boundary, including faults inside a partly retired simple slot. The exact
sequence prefix is propagated consistently to Rename, IQ, and LSQ.

## Build

```sh
scons build/RISCV/gem5.opt --gold-linker -j64
```

Planner and slot boundary tests are retained locally as development aids
and are not registered in the project build or included in the PR.
The full-system validation commands below use the normal simulator build.

## Full-system validation

Set the reference explicitly; a normal GCPT carries its own restorer. The
following workload is a raw image and therefore uses `--raw-cpt`.

```sh
export GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so
build/RISCV/gem5.opt --outdir=/tmp/crob-hybrid \
    --debug-flags=ROB,CommitRate --debug-file=rob.log \
    configs/example/kmhv3.py --raw-cpt \
    --generic-rv-cpt=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin \
    --param="system.cpu[0].RobCompressPolicy='hybrid'" \
    --maxinsts=50000 --mem-type=SimpleMemory
```

Repeat with a separate output directory and
`--param='system.cpu[0].commitWidth=1'` to check one-entry retirement. Add
`--rob-walk-policy=Rollback --param='system.cpu[0].squashWidth=1'` to test
incremental squash, or `--rob-walk-policy=NaiveCpt` for snapshot recovery.

ROB tracing records allocation ids, slots, member order, removals, and
downgrades. These records and the allocation/retirement statistics can be
used to inspect physical-entry width, recovery boundaries, and member
conservation. CommitRate reports DynInst retirements and alone cannot prove
physical-entry width.

Value-prediction validation must exercise actual mispredictions. Check that
`system.cpu.valuePred.VPpredicted` and
`system.cpu.commit.squashDueToValuePrediction` are nonzero and that difftest
passes. Check `childOffered` and `childSelected` for both children to confirm
that both methods participate. Use `satCounterBits=1` for IdealConstantLVP
and `logMaxConfidence=1`, `thresholdPercent=0.0` for EStride to shorten
warmup and exercise recovery; these settings are for validation, not performance
comparison. Repeat with
`--param='system.cpu[0].enableSelectiveVPFlush=True'` to cover selective
cancellation and inspect whether the squash fallback was exercised. A run
without mispredictions does not validate the recovery path.

## Correspondence limits

AUIPC is C through `IntJpOp`; ORI-encoded PREFETCH hints are S through
`IntAluOp`. RVC and retained fusion use their decoded semantics. The decoder
does not implement the document's CBO instruction families as cache-block
operations; this change does not add ISA encodings. Other unimplemented or
uncovered instruction classes remain N according to visible flags/faults and
the conservative default. Only enqueue-time faults affect classification;
later faults use the existing precise per-DynInst recovery path.

There is no explicit RAB or new pipeline stage. Physical entry and slot
metadata model allocation, retirement bandwidth, and redirect selection;
architectural side effects and precise faults remain per DynInst. A fused DynInst can represent two architectural
instructions, so allocation and quota statistics differ from `committedInsts`.
No claim of cycle or performance equivalence to XiangShan RTL or the paper is
made. SPEC checkpoint campaigns and interrupt-specific stress are not included
in this initial validation.
