# Decode fusion compaction regression

These tests execute real RISC-V instructions through the Kunminghu pipeline.
They do not replace Decode with a Python model or inject a test-only instruction
queue. `fusion.S` contains three self-checking bare-metal workloads:

| Variant | Behavior exercised |
| --- | --- |
| `scalar` | Dense eight-pair blocks, compressed 2+2-byte instructions, LUI/ADDI fusion, divide and dependent-load pressure |
| `control` | Conditional and direct branches, call/return, wrong-path store checks, a precise illegal-instruction trap and MRET |
| `vector` | VSET, scalar/vector interleaving, vector ALU and store results for both e64 lanes |

All checks must pass before the program executes the XiangShan exit instruction.
Failure loops until the instruction limit; reaching that limit does **not** pass.
No AM runtime or checkpoint restorer is needed. The GNU RISC-V binutils must
support `-march=rv64gcv`. The suite uses 256 MB of memory and the portable
`DDR3_1600_8x8` model so it also works in builds without DRAMsim3. Override this
with `--mem-type` if needed. These memory settings are identical across each
comparison and are only for correctness tests, not for performance scoring.

## Run

From the repository root, resolve the reference model pinned by this checkout:

```sh
python3 util/nemu_ref/resolve.py normal
```

Use the printed path below. Use `gem5.opt` or `gem5.debug` with tracing enabled;
`gem5.fast` does not provide the per-cycle Decode samples.

```sh
python3 tests/test-progs/decode-fusion/run.py \
  --gem5 build/RISCV/gem5.opt \
  --ref-so /absolute/path/to/riscv64-nemu-interpreter-so \
  --outdir /tmp/decode-fusion-regression \
  --tools
```

The output directory must not already exist. Every invocation retains its argv
in `command.json`, simulation log, configuration, statistics and Decode log.
`report.json` records results, coverage observations, and any failure. Difftest
is enabled by default and its reference must be supplied with `--ref-so` or
`GCBV_REF_SO`. Use the normal reference for raw images, without memory dedup.
`--no-difftest` explicitly selects self-check-only execution and records this
limitation in the report.

The default suite runs:

1. Each workload with the new switch False and True, comparing retired
   architectural instruction counts and requiring the success exit.
2. The control workload with predecode disabled in both switch settings. The
   True run must actually record a Decode redirect, exercising `selfSquash`
   and its FIFO flush; functional success without that observation is insufficient.
3. Scalar runs with scan width 8 and FIFO capacity 24 to exercise the supported
   scan and backpressure boundaries, in addition to the normal 16/40 settings.
4. A generated 512-instruction ChampSim trace with False and True. Both must
   exit normally, have identical simulated work/time and legacy Decode
   statistics, and leave all compaction counters zero.
5. Startup rejection checks for load fusion, constant folding, immediate move
   elimination, an insufficient predecode delay, FIFO capacity below the
   in-flight reserve, and a zero scan width. The diagnostic must identify the
   rejected parameter; an unrelated startup error cannot pass the test.

`--tools` also runs the scalar workload with O3PipeView and PerfCCT enabled,
checks committed database records, and invokes the repository's existing
`util/o3-pipeview.py` and `util/perfcct.py` parsers for both switch settings.
This checks format and parser compatibility; it does not add a new visualization
of fused instruction ownership or claim that the existing tools display both
original instructions as one fused timeline.

When a binary from the unmodified, **same** base commit is available, pass
`--baseline-gem5 /absolute/path/to/baseline/gem5.opt`. The runner then compares
its simulated ticks, architectural instructions, backend operations, CPU cycles,
and legacy Decode statistics with the modified binary's False runs. Both use
this checkout's unmodified `kmhv3.py` configuration. This is a local regression,
not a SPEC score comparison.

## Production invariants and coverage limits

Every recorded compaction cycle must satisfy:

```text
raw + discarded <= configured scan width
output <= 8
output == raw - fused
2 * fused <= raw
queued <= configured FIFO capacity
```

Aggregate counters must also conserve valid input and output. Each True fixture
must actually fuse at least one pair; merely booting successfully is insufficient.
Trace and False runs must never execute the compaction path.

Dense layout and backend pressure create opportunities for accumulated input,
but the test does not assume a particular FTQ partition, predictor history, or
queue occupancy. The report explicitly says whether a 16-raw/8-pair cycle,
cross-Fetch-bundle fusion, and an invalid-entry discard were observed. Add
`--require-16-pairs` when that boundary must be observed for acceptance; absent
coverage then fails instead of silently passing. Merely placing 16 instructions
in an assembly block does not establish that one Decode cycle consumed them.

These workloads alone cannot prove every synthetic FIFO state (such as an
invalid entry exactly between a particular candidate pair), every same-cycle
redirect ordering, SMT execution, thread exit/takeover, or all FIFO drain states.
They complement targeted internal tests and checkpoint difftest; do not report
those additional cases as covered solely by this suite. Short vector tests also
do not replace full vector/exception regression.

For an individual fixture, assemble and run manually:

```sh
riscv64-linux-gnu-as -march=rv64gcv --defsym TEST_CONTROL=1 \
  -o /tmp/decode-control.o tests/test-progs/decode-fusion/fusion.S
riscv64-linux-gnu-ld -Ttext=0x80000000 \
  -o /tmp/decode-control.elf /tmp/decode-control.o
riscv64-linux-gnu-objcopy -O binary \
  /tmp/decode-control.elf /tmp/decode-control.bin
build/RISCV/gem5.opt --outdir=/tmp/decode-control-run \
  --debug-flags=Decode --debug-file=decode.log \
  configs/example/kmhv3.py --raw-cpt \
  --generic-rv-cpt=/tmp/decode-control.bin --mem-size=256MB \
  --mem-type=DDR3_1600_8x8 \
  --enable-difftest --difftest-ref-so=/absolute/path/to/reference.so \
  --warmup-insts-no-switch=0 --maxinsts=20000 \
  --param=system.cpu[0].enableDecodeFusionCompaction=True
```

Omit `--defsym` for scalar or use `--defsym TEST_VECTOR=1` for vector. Keep the
maximum instruction limit and check the exit cause, not only the host process
exit code.

## Optional fused-fault replay self-check

`fusion-fault.S` exercises gem5's reserved `C.LUI t0,0` followed by `ADDI`.
It checks that a faulting fusion is re-executed without fusion, traps exactly
once at the original two-byte instruction, and resumes with the expected
register result. This case is intentionally separate from the default suite:
the pinned NEMU treats this reserved encoding differently, and original
baseline, False and True all exhibit the same difftest disagreement.

```sh
riscv64-linux-gnu-as -march=rv64gc -o /tmp/fusion-fault.o \
  tests/test-progs/decode-fusion/fusion-fault.S
riscv64-linux-gnu-ld -Ttext=0x80000000 \
  -o /tmp/fusion-fault.elf /tmp/fusion-fault.o
riscv64-linux-gnu-objcopy -O binary \
  /tmp/fusion-fault.elf /tmp/fusion-fault.bin
build/RISCV/gem5.opt --outdir=/tmp/fusion-fault-true \
  --debug-flags=Decode,Commit --debug-file=decode.log \
  configs/example/kmhv3.py --raw-cpt \
  --generic-rv-cpt=/tmp/fusion-fault.bin --mem-size=256MB \
  --mem-type=DDR3_1600_8x8 --disable-difftest \
  --warmup-insts-no-switch=0 --maxinsts=20000 \
  --param=system.cpu[0].enableDecodeFusionCompaction=True
```

Require the normal `m5_exit instruction encountered` success with code zero,
and exactly one `Fault on fusion instruction, re-execute without fusion` in
`decode.log`. Repeat with False, and with the unchanged baseline binary while
omitting the new parameter. Label this result **self-check only**, not difftest.
