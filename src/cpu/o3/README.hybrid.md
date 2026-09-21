# Hybrid CROB validation

The model keeps per-DynInst execution and architectural retirement, with a
persistent physical-entry deque alongside the flat ROB instruction list.
Each entry records former/latter remaining-member counts and runtime type
(`NORMAL`, `CC`, `CS`, `SC`); each DynInst carries an entry id and slot bit.
The temporary planner retains `NORMAL-S/C/N` distinctions for classification
and allocation statistics.

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

The configuration supplies one thread, no value predictor, no Load fusion or
Rename elimination, group length 8, rename width 8, and physical commit width
8. Existing non-Load fusion remains active. Other `--param` assignments can
override group size and entry width afterward. `numROBEntries` is physical
capacity (352 in this configuration).

Planning is O(renameWidth); insertion and removal accounting is O(1) per
member. Commit scans one bounded entry when selecting it, and handles at most
`commitWidth * CROB_instPerGroup` members per cycle. Redirect setup scans at
most the bounded ROB to find the retained prefix; the recovery walker retains
its existing configured per-cycle width. Full invariant scans only run with
ROB debug tracing enabled.

## Slot-aware recovery

| Redirect slot | Include redirect slot | Surviving slots in target entry |
|---|---|---|
| former | yes | none |
| former | no | former |
| latter | yes | former |
| latter | no | former and latter |

All younger entries are removed. Removing the last latter member downgrades
the target to `NORMAL` only if a nonempty former survives; its id is preserved
and it is never paired with a new allocation. Full trap/TC flushes and
already-retired squash-after anchors retain the architectural sequence
boundary, including faults inside a partly retired simple slot. The slot
selector produces the same retained sequence prefix for Rename, IQ, and LSQ.

## Automated boundary tests

```sh
scons build/RISCV/gem5.opt build/RISCV/cpu/o3/rob_hybrid.test.opt --gold-linker -j64
build/RISCV/cpu/o3/rob_hybrid.test.opt
```

The ten gtests cover planner examples, type/length legality, independent
batches, exhaustive S/C/N inputs up to length eight, all four squash cases,
multimember slots, incremental latter clearing, whole-entry deletion, and
partially retired former slots.

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
