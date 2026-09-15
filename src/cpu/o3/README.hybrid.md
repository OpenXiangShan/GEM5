# Hybrid CROB validation

The model keeps per-DynInst execution, faults and Rename history recovery.
Physical capacity uses the remaining-member counts in `threadGroups`; segment
types exist only in a temporary admission plan. Planning is O(renameWidth),
member insertion/removal uses O(1) accounting, and `ROB` debug tracing enables
the full group-count sum check. `commitWidth` retains the existing expanded
group window, including its squashed-instruction draining semantics.

Use `configs/example/kmhv3_hybrid.py` for the initial experiment: one thread,
no value predictor, no Load fusion or Rename elimination, group length 8,
rename width 8, group commit window 8 and successful-retirement quota 16.
Existing non-Load fusion remains active. The default `commitInstWidth=0`
preserves the old retirement loop's behavior for every compression policy.

## Automated boundary tests

```sh
scons build/RISCV/gem5.opt build/RISCV/cpu/o3/rob_hybrid.test.opt --gold-linker -j64
build/RISCV/cpu/o3/rob_hybrid.test.opt
python3 src/cpu/o3/hybrid_method_test.py
```

The five gtests check the specified examples, each final type, group limits,
batch independence, and segment legality for all S/C/N sequences of length
0 through 8 at limits 1, 2, 3 and 8. The seven method tests compile the actual
production methods with small substitutes for CPU/DynInst interfaces. They
cover classification precedence, one free entry admitting eight S instructions,
failed admission without consumption/statistics, replanning after squash,
all-squashed and mixed windows without refill, group readiness, partial
retirement, squashed-head draining, and partial/full tail squash. These fixtures
do not substitute for execution, decoder, or Rename recovery integration tests.

## Full-system validation

Set the reference explicitly; a normal GCPT carries its own restorer. The
following workload is a raw image and therefore uses `--raw-cpt`.

```sh
export GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so
build/RISCV/gem5.opt --outdir=/tmp/crob-hybrid16 \
    --debug-flags=ROB,CommitRate --debug-file=rob.log \
    configs/example/kmhv3_hybrid.py --raw-cpt \
    --generic-rv-cpt=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin \
    --maxinsts=50000 --mem-type=SimpleMemory
python3 src/cpu/o3/hybrid_trace_check.py /tmp/crob-hybrid16/rob.log
```

Repeat with a separate output directory and
`--param='system.cpu[0].commitInstWidth=0'` for the unlimited-quota case.
The trace checker reconstructs allocations and removals, verifies statistics
and conservation, checks retirement quotas, and reports partially retired
groups. It requires a complete trace from tick zero without statistics resets.

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
    configs/example/kmhv3_hybrid.py --raw-cpt \
    --generic-rv-cpt=/tmp/crob-exception.bin --maxinsts=10000 \
    --mem-type=SimpleMemory
python3 src/cpu/o3/hybrid_trace_check.py /tmp/crob-exception/rob.log
```

## Results from 2026-09-14

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

There is no explicit RAB, former/latter storage, segment squash atomicity, or
new pipeline stage. A fused DynInst can represent two architectural
instructions, so allocation and quota statistics differ from `committedInsts`.
No claim of cycle or performance equivalence to XiangShan RTL or the paper is
made. SPEC checkpoint campaigns and interrupt-specific stress are not included
in this initial validation.
