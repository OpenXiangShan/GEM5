# Single-active-thread SMT experiment

This experiment restores an existing two-context A+A GCPT checkpoint, parks
hardware thread 1, and measures hardware thread 0 with `numThreads=2` and the
configured SMT resource policies. It is disabled unless `--smt-single-thread`
is present. The normal A+A path continues to use its existing instruction and
warmup limits.

## Run

Use the same checkpoint, memory configuration, restorer and multi-context
reference model as a working A+A run. For example, append these options to the
usual `configs/example/smt_idealkmhv3.py` command:

```sh
--smt-single-thread \
--smt-park-after-insts=1000000 \
--smt-single-thread-warmup-insts=20000000 \
--smt-single-thread-measure-insts=20000000
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--smt-single-thread` | Enable this experiment; requires one two-thread O3 CPU without SpMT or CPU switching. | Off |
| `--smt-park-after-insts` | T0 instruction count before requesting the park, including checkpoint restorer instructions. | 1,000,000 |
| `--smt-single-thread-warmup-insts` | Additional T0 instructions after T1 is fully parked and before statistics reset. | `--warmup-insts-no-switch`, or 0 |
| `--smt-single-thread-measure-insts` | T0 instructions measured after that statistics reset. | `--maxinsts`; one of these limits must be positive |

In this mode `--maxinsts`, when used as the fallback, denotes the measurement
length after warmup. The native any-thread instruction limit and automatic
warmup reset are not armed. Set the explicit single-thread limits to avoid
ambiguity when reusing CI arguments. Instruction-count exit events may include
a few extra instructions from the final commit group; the JSON records the
actual count.

For a GCPT slice, both architectural PCs must be outside the configured GCPT
restorer memory range at the park request point. If this check fails, increase
`--smt-park-after-insts`. Keep the working A+A restorer setup; an embedded
restorer can be selected explicitly with `--gcpt-restorer=` when the checkpoint
has a compatible one. Raw binaries skip this GCPT-specific PC check.

## Execution and outputs

1. Both contexts execute the checkpoint restorer and initial workload prefix.
2. Commit chooses a complete architectural instruction boundary in T1 and
   squashes younger instructions. T1 stays active while this cleanup runs.
3. T1 stops generating predictions and fetch/dispatch work. Its committed
   stores flush, and its ROB, LSQ, dynamic instruction list and fetch pipeline
   must become empty. Inter-stage feedback is allowed to settle before the
   final speculative FTQ target is released.
4. `ThreadContext::suspend()` removes T1 from the active lists. Experimentally
   parked T1 ignores wakeups and further activation attempts for this run.
5. T0 warms up, statistics reset, and T0 executes the measurement interval.

The output directory contains `smt-single-thread.json`, with phase ticks,
per-thread cumulative instruction counts, and actual measurement instruction
and tick deltas. `stats.txt` contains the measurement statistics. The runner
fails if T1's committed instruction count changes during warmup or measurement,
if an unexpected exit occurs, or if the simulation tick limit is reached early.

Check these results:

- `numThreads` is still 2 in `config.ini`.
- The log contains `SMT thread parked` and `activeThreads=1`.
- `measurement_insts[1]` in the JSON is zero.
- `system.cpu.committedInsts::1` and `system.cpu.committedOps::1` are zero.
- T0 commits the requested measurement interval and difftest remains enabled.

## Interpretation and scope

The experiment preserves T1's architectural rename mappings and existing SMT
allocation policies, including reserved quotas and predictor table partitions.
It releases in-flight speculative work but does not free architectural register
state or modify resource-sharing algorithms. Intentional reservations and fixed
partitions must be distinguished from allocation defects when comparing with a
one-thread configuration. Compare identical physical capacities, widths,
workload regions and warmup lengths.

The control state is intended for this fixed T0-only experiment; resuming T1 or
serializing/restoring an in-progress parking experiment is not supported. WFI
semantics, checkpoint contents and the reference implementation are unchanged.

## Local smoke validation

On 2026-10-26, `build/RISCV/gem5.opt` was built with
`scons build/RISCV/gem5.opt --gold-linker -j64` and tested using the dual-context
SPEC2006 `bzip2_chicken/728` slice with a multi-context NEMU reference and memory
deduplication enabled. With a 1,000,000-instruction T0 prefix, 20,000 additional
warmup instructions and a 100,000-instruction measurement, T1 stayed at
1,064,195 committed instructions after parking. The measured increments were
T0=100,001 and T1=0. The default A+A smoke run also exited normally with both
threads committing. These short runs validate control flow and difftest, not
SPEC-wide performance or every SMT allocation policy.
