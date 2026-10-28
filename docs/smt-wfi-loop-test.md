# SPEC plus a continuously executing WFI loop

This experiment keeps the original CPU model and runs a dual-context SPEC2006
A+A checkpoint with T0 executing SPEC and T1 continuously committing a software
`wfi; j loop` sequence. It supersedes the wait-state experiment from 798d15bf7f.

## Meaning of the experiment

The original RISC-V decoder checks WFI privilege and otherwise implements it as
a no-op. T1 therefore repeatedly fetches, executes and commits WFI and the
backward jump. This is intentional. T1 continues competing for frontend,
predictor, rename, ROB/IQ, execution and commit resources under the existing
policies. This experiment includes the interference from the idle loop; it does
not represent a thread that stops consuming pipeline resources.

There are no changes under `src/`, no new CPU parameters or WFI flags, no
ThreadContext suspension, no scheduler exclusions, and no watchdog exemption.
Both configured hardware threads retain their original model behavior.

## Software restorer

The dedicated restorer redirects hart 1 after the shared restore barrier and
before its integer-state restoration/mret. Hart 0 retains the original restore
path. Hart 1 stays in machine mode, masks interrupts, fences and enters:

```asm
loop:
    wfi
    j loop
```

Only this software is changed. There is no need to rewrite the SPEC payload or
regenerate every checkpoint. The original LibCheckpoint checkout is copied into
an isolated build directory and is left untouched. Validated source:
`OpenXiangShan/LibCheckpoint` at
`2115c400af664ba06c57114db39a777ee7e5f1d0`, with nanopb initialized.

```sh
python3 util/xs_scripts/smt_wfi/build_restorer.py \
  --source /path/to/LibCheckpoint \
  --output /shared/path/smt-wfi-loop-restorer
```

The builder requires make, bash and a compatible RISC-V cross-toolchain
(default prefix `riscv64-linux-gnu-`). It emits `gcpt-wfi.bin`, an ELF and
`manifest.json` with source revision, SHA256 and loop PC. PIE and build-id
insertion are disabled so the binary begins at the required restorer header.
Use `dual_core` mode for the legacy QEMU A+A checkpoints. The software binary
may match the old restorer byte-for-byte; the CPU and execution semantics are
what changed, and this version has an explicit `wfi-loop` manifest.

## T0 measurement and score

Use `--smt-wfi-loop-test`, not the removed `--smt-wfi-test` option. The new option
selects a Python measurement driver, not a CPU feature. Only T0 instruction
counts control warmup and termination. This prevents T1's loop from ending the
run early or resetting statistics at the wrong point.

The existing `--warmup-insts-no-switch=20000000 --maxinsts=40000000` options keep
their absolute T0 instruction-count semantics. Measurement starts after at least
`--smt-wfi-loop-restore-insts=1000000` T0 instructions. The restorer's sibling
`manifest.json` is verified before simulation; override its location with
`--smt-wfi-loop-manifest`. Output contains two statistics blocks and a
`smt-wfi-loop.json` report with phase counts, ticks and the binary hash.

Raw gem5 statistics are left intact:

```text
totalIpc = (T0 instructions + T1 loop instructions) / CPU cycles
T0 IPC   = T0 instructions / CPU cycles
```

Only the second quantity is suitable for reporting SPEC performance here.
`score_t0.py` validates every job and constructs a dedicated CSV with T0 IPC,
CPI, instruction count, and the shared elapsed CPU cycles. It then invokes the
existing data processor's SPEC weighted-score calculation. T1 counts and total
IPC remain separate diagnostic columns. No shared data-processor files, raw
stats or reference times are modified. The T0 score has no thread-count factor.

```sh
python3 util/xs_scripts/smt_wfi/score_t0.py \
  --archive /path/to/perf/spec_all \
  --cluster /path/to/cluster.json \
  --data-proc /path/to/gem5_data_proc \
  --output-dir /path/to/perf/t0-score
```

The data processor needs its usual Python dependencies and import path.
This score excludes T1's instructions but includes any real slowdown T1 causes
T0. Raw cache, predictor and other aggregate statistics still include both
threads and must not be labeled as T0-only PMU statistics.

## Smoke tests

```sh
python3 util/xs_scripts/smt_wfi/run_smoke.py \
  --checkpoint /path/to/dual-core-slice.gz \
  --restorer /shared/path/smt-wfi-loop-restorer/gcpt-wfi.bin \
  --ref-so /path/to/multi/riscv64-nemu-interpreter-so \
  --output /path/to/fresh-output
```

Default case `loop` runs T0 from approximately 1.3M to 1.8M instructions with
multi-context difftest enabled. Both threads must commit during measurement.
`--case trace` checks repeated commits at the WFI and JAL PCs using the existing
commit-dump facility. `--case normal` runs the original embedded-restorer A+A
case. `--case timeout` verifies incomplete simulation fails. The output directory
must be new. Standalone validation:

```sh
python3 util/xs_scripts/smt_wfi/validate.py --output /path/to/fresh-output
```

## CI

In Manual Performance Test, select `smt-single-thread-wfi`, configuration
`smt_idealkmhv3.py`, and the matching `gcc12-spec06-smt-*` benchmark. Extra args:

```text
--smt-wfi-loop-test --gcpt-restorer=/shared/path/smt-wfi-loop-restorer/gcpt-wfi.bin
```

The binary and sibling manifest must be readable by every distributed worker.
CI validates loop progress before scoring and invokes the T0-only score path
only for this option. Default A+A and ordinary ST jobs use the original path.
This does not automatically launch a full CI run.

Compare T0 scores using matched checkpoint contents, coverage, compiler,
warmup/measurement intervals, CPU widths and predictor/cache settings. In
particular, a difference from ST can include both existing SMT allocation policy
and actual contention from the WFI loop.
