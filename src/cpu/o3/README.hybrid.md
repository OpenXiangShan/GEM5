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
python3 src/cpu/o3/hybrid_trace_check.test.py
```

The ten gtests cover planner examples, type/length legality, independent
batches, exhaustive S/C/N inputs up to length eight, all four squash cases,
multimember slots, incremental latter clearing, whole-entry deletion, and
partially retired former slots. The Python tests reject over-width entry
retirement, incorrect retained boundaries, and a walker removing retained
members; they also cover multi-instruction commit and overlapping full flush.

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
python3 src/cpu/o3/hybrid_trace_check.py /tmp/crob-hybrid/rob.log
```

Repeat with a separate output directory and
`--param='system.cpu[0].commitWidth=1'` to check one-entry retirement. Add
`--rob-walk-policy=Rollback --param='system.cpu[0].squashWidth=1'` to test
incremental squash, or `--rob-walk-policy=NaiveCpt` for snapshot recovery.

The trace checker reconstructs allocation ids, slots, member order, removals,
and downgrades; it checks physical-entry width, recovery boundaries,
statistics, and member conservation. Use a complete ROB,CommitRate trace from
tick zero without statistics resets. CommitRate alone cannot prove entry
width.

`testdata/hybrid_exception.S` repeatedly executes FP with FS disabled (a fault discovered while
executing an S instruction), ECALL, a trap handler and MRET. The NEMU reference
checks architectural state and the ROB trace checks member accounting through
precise exception recovery.

```sh
riscv64-linux-gnu-gcc -nostdlib -nostartfiles -static \
    -march=rv64imafd_zicsr -mabi=lp64d -Wl,-Ttext=0x80000000 \
    -Wl,--build-id=none src/cpu/o3/testdata/hybrid_exception.S -o /tmp/crob-exception.elf
riscv64-linux-gnu-objcopy -O binary /tmp/crob-exception.elf /tmp/crob-exception.bin
build/RISCV/gem5.opt --outdir=/tmp/crob-exception \
    --debug-flags=ROB,CommitRate --debug-file=rob.log \
    configs/example/kmhv3.py --raw-cpt \
    --generic-rv-cpt=/tmp/crob-exception.bin --maxinsts=10000 \
    --param="system.cpu[0].RobCompressPolicy='hybrid'" \
    --mem-type=SimpleMemory
python3 src/cpu/o3/hybrid_trace_check.py /tmp/crob-exception/rob.log
```

## Results from 2026-09-20

- Optimized build, ten gtests, and seven Python trace-checker tests passed.
- CoreMark/NEMU: 50,000 architectural instructions; maximum 31 DynInst
  retirements and eight physical entries per cycle. Trace recorded 122
  surviving-entry downgrades and 145,006 invariant checks.
- Width-one commit and width-one Rollback recovery: CoreMark 20,000
  instructions passed; maximum eight DynInsts in one physical entry per cycle,
  with 120 downgrades. NaiveCpt recovery also passed 20,000 instructions.
- The exception workload passed 10,000 instructions and 42,004 invariant
  checks, including faults discovered inside simple slots.
- Four legacy policies passed 10,000-instruction CoreMark/NEMU runs. All
  compared pre-existing non-host stats matched for kmhv3/kmhv2/MohBoE. For
  none, only `decodeEfficiency` differed; DecodeStats reads `CPU::issueWidth`
  before initialization, an existing unrelated issue. Cycle/retirement stats
  and all other compared counters matched.

`testdata/hybrid_replay.S` delays an older store address with integer division
and checks a younger load against the stored value. Build it with the same
commands as `hybrid_exception.S` (substitute the file/output names) and run
both `--param="system.cpu[0].mdp_violation_timing='atResolve'"` and
`--param="system.cpu[0].mdp_violation_timing='atCommit'"`. Inspect `Hybrid squash`
records with `itself=1` to verify that an actual memory replay was exercised.
The default assembly variant includes a simple former member before the load
and exercised latter-slot replay in both modes (30,007 successful DynInsts).
Compile with `-DHYBRID_REPLAY_SIMPLE_SLOT=0` to exercise former-slot replay.
Together with CoreMark branch redirects, these runs cover all four slot cases.

The full unit-test command stopped at six existing SocketTest cases because
socket creation is forbidden by the sandbox (`Operation not permitted`).
These are independent of the Hybrid tests. SPEC and targeted interrupt or
strictly ordered load stress are not covered by these runs.

## Regression after removing the obsolete DynInst quota

The parameter, limit checks, assertion, and quota-full statistic were removed
for all policies. Optimized build, ten C++ tests, seven Python checker tests,
and modified-region style checks passed. Hybrid CoreMark (50,000 instructions)
and exception recovery (10,000 instructions) passed NEMU and trace validation;
all compared existing non-host stats matched the preceding entry-model build
apart from the intentionally removed statistic. Four non-Hybrid policies each
passed 10,000-instruction CoreMark/NEMU runs; only the already noted
`decodeEfficiency` initialization variation appeared for none and kmhv2.
Logs and comparisons: `/tmp/crob-remove-inst-quota-20260920/`.

## Historical results from 2026-09-14 (previous quota model)

- Optimized simulator build, five gtests, seven method tests, Python syntax
  checks and the repository's modified-region style checks passed.
- CoreMark with NEMU difftest passed at both quotas. At quota 16: maximum 16
  successful retirements, 756 quota-full cycles, 169 cycles stopping inside a
  group, and maximum eight groups accessed. All 145,010 invariant checks and
  allocation statistics matched the trace: 67,237 DynInsts / 30,709 groups =
  2.189488. At quota 0: maximum 31 retirements and zero quota-full samples.
- The exception workload passed NEMU difftest for 10,000 committed instructions,
  including 1,333 trap squashes and 42,004 group-invariant checks.
- The four legacy policies (`none`, `kmhv3`, `kmhv2`, `MohBoE`) passed CoreMark
  difftest and matched the preserved pre-change binary's cycle/retirement stats.
  About 6,500 non-host statistics were compared per policy. An initial
  `decodeEfficiency` difference was an existing uninitialized read of
  `CPU::issueWidth` during `DecodeStats` construction; a repeat matched all
  old statistics. That unrelated issue is unchanged.
- The full `scons build/RISCV/unittests.opt -j64 --unit-test` run stopped at six
  existing socket tests because the sandbox disallows socket creation. The
  targeted Hybrid tests passed independently.

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
