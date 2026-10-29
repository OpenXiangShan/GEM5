# SPEC with its SMT sibling waiting in WFI

This experiment restores a dual-context SPEC2006 A+A checkpoint, runs SPEC on
T0, and lets T1 retire one WFI and wait. The O3 model now implements this behavior
for either hardware thread, independently of the measurement option.

## Retirement and cleanup

Previously the decoder checked WFI privilege and then treated it as a no-op.
The dedicated restorer consequently kept committing `wfi; j loop`, competing
with SPEC for frontend, predictor, IQ and commit resources.

WFI now has non-speculative, serialize-after, squash-after and quiesce flags.
Its existing privilege checks still run before architectural retirement. A
faulting or squashed WFI does not enter the wait state. On successful retirement:

1. Commit marks the thread as cleaning and immediately prohibits new fetch,
   pre-dispatch and branch prediction for that thread.
2. The existing squash-after path removes younger instructions and rolls back
   speculative state. Already committed stores are allowed to finish. Cleanup
   waits for the thread's ROB, LSQ, instruction list, frontend requests and
   inter-stage buffers; it does not drain or stop the other thread.
3. Once cleanup completes, the thread enters waiting. It has no continuing
   instruction or predictor traffic until a valid interrupt wakes it.

Stopping fetch occurs in the WFI retirement cycle. Physical cleanup follows the
existing pipeline delays and squash bandwidth; it is not an instantaneous
zero-latency deletion of every outstanding request. An I-cache request sent
before retirement can still finish during cleanup.

The thread remains in `activeThreads` with an Active ThreadContext. Its
architectural registers, rename mappings and hardware allocation identity are
retained. Existing SMT ROB/IQ/LSQ/FTQ policies are unchanged: two identities
remain allocated even though only one thread is runnable. Dynamic borrowing
continues to behave according to its existing policy.

Only fully waiting threads are exempt from the commit-progress watchdog.
Cleanup and the running sibling remain checked. Waking resets the waiting
thread's watchdog interval.

## Interrupts and state lifetime

Wake eligibility uses the live local interrupt-pending and interrupt-enable
registers. A locally enabled interrupt wakes WFI even when global mstatus.MIE is
clear. A masked interrupt does not wake it. An eligible interrupt that arrives
during cleanup is latched until cleanup completes, even if subsequently
cleared. Resume starts at the architectural PC following WFI.

This change supplies per-thread WFI wake eligibility. It does not redesign the
repository's existing interrupt trap-routing path, which is primarily T0 based.
The interrupt smoke test uses T1 MSIP with global MIE clear to test waking
independently of interrupt trap delivery.

Native O3 checkpoints preserve the waiting flag, PC, pending wake and entry/wake
counters, with optional fields for compatibility with older checkpoints.
Drained O3-to-O3 CPU takeover also transfers this state. The targeted GCPT tests
below do not exercise native checkpoint restoration or CPU switching.

## Software restorer

The existing dedicated restorer is reusable. It redirects hart 1 after the
shared restore barrier, before integer-state restoration/mret. Hart 0 follows
its original path. Hart 1 remains in machine mode, masks interrupts, fences and
enters:

```asm
loop:
    wfi
    j loop
```

Under the corrected O3 model, WFI commits once and its following jump is
squashed. If an interrupt wakes the thread, it executes the jump and may enter
WFI again. No SPEC payload changes or checkpoint regeneration are required.

Build the restorer from the supported LibCheckpoint source revision
`2115c400af664ba06c57114db39a777ee7e5f1d0`, with nanopb initialized:

```sh
python3 util/xs_scripts/smt_wfi/build_restorer.py \
  --source /path/to/LibCheckpoint \
  --output /shared/path/smt-wfi-loop-restorer
```

The builder copies the source into an isolated directory and requires make,
bash and a compatible RISC-V cross-toolchain. Use `dual_core` for the legacy
QEMU A+A checkpoints. Output includes `gcpt-wfi.bin`, an ELF and `manifest.json`
with source revision, binary SHA256 and WFI PC. The manifest's `wfi-loop` label
identifies this software artifact; measurement reports use `wfi-wait`.

## Measurement and score

Use `--smt-wfi-test`; `--smt-wfi-loop-test` remains a compatibility alias. These
options select the Python experiment driver and enable `wfiExitOnWait`, an exit
notification after cleanup. They do not enable or disable the CPU's WFI logic.

The driver first requires T1 to reach waiting at the manifest's WFI PC. Then
only T0 instruction counts control warmup and termination. Existing
`--warmup-insts-no-switch=20000000 --maxinsts=40000000` options retain their
absolute T0 instruction-count semantics. Measurement starts after at least
`--smt-wfi-loop-restore-insts=1000000` T0 instructions, outside the restorer.

The binary hash is checked against the sibling `manifest.json`; its location
can be overridden with `--smt-wfi-loop-manifest`. Output retains two statistics
blocks and the filename `smt-wfi-loop.json`. All three phase snapshots must show
two allocated threads, one runnable thread, one T1 WFI entry, zero wakes and a
constant T1 instruction count. During measurement T0 must commit instructions;
T1 commits and new predictions must both be zero.

`score_t0.py` validates these conditions and uses T0 instructions divided by
elapsed shared CPU cycles. It passes T0 CPI to the existing weighted SPEC score
calculation without a thread-count multiplier. Raw statistics and the common
data processor are unchanged. In this experiment total IPC equals T0 IPC after
cleanup because T1 contributes no measured instructions.

```sh
python3 util/xs_scripts/smt_wfi/score_t0.py \
  --archive /path/to/perf/spec_all \
  --cluster /path/to/cluster.json \
  --data-proc /path/to/gem5_data_proc \
  --output-dir /path/to/perf/t0-score
```

## Smoke tests

```sh
python3 util/xs_scripts/smt_wfi/run_smoke.py \
  --checkpoint /path/to/dual-core-slice.gz \
  --restorer /shared/path/smt-wfi-loop-restorer/gcpt-wfi.bin \
  --ref-so /path/to/multi/riscv64-nemu-interpreter-so \
  --output /path/to/fresh-output
```

Cases use a new output directory and multi-context difftest:

- `loop` (default): measure T0 around 1.3M--1.8M instructions; require T1 zero
  progress and zero new predictions throughout measurement.
- `trace`: require exactly one WFI commit and zero following-loop JAL commits.
- `normal`: run the embedded-restorer A+A case with both threads progressing.
- `timeout`: reject an experiment that stops before WFI/measurement readiness.
- `masked`: post masked T1 MSIP and verify continued waiting and T0 progress
  beyond the usual watchdog interval.
- `wake`: enable only local MSIP in an isolated restorer copy, wake T1 with
  global MIE clear, then inject a short interrupt during its next WFI cleanup.
  Verify both wakes, the resume PC, and exactly two JAL/WFI pairs before the
  third wait. The shared restorer is never modified.

Standalone validation:

```sh
python3 util/xs_scripts/smt_wfi/validate.py --output /path/to/fresh-output
```

## CI

In Manual Performance Test select `smt-single-thread-wfi`, configuration
`smt_idealkmhv3.py` and the matching `gcc12-spec06-smt-*` benchmark. Extra args:

```text
--smt-wfi-test --gcpt-restorer=/shared/path/smt-wfi-loop-restorer/gcpt-wfi.bin
```

All workers must see the restorer and its sibling manifest. CI validates the
waiting experiment before invoking T0 scoring. Ordinary ST and default A+A use
their existing score path. No IQ Watermark override is required to suppress T1
loop traffic: T1 stops creating that traffic after WFI retirement and cleanup.

Compare performance against ST using matched checkpoint coverage, compiler,
warmup/measurement intervals, widths and predictor/cache settings. Any observed
gap still needs attribution to the remaining SMT policies and resource layout.
