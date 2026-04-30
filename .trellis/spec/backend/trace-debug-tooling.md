# Trace Debug Tooling

> Executable contracts for trace-focused debug helpers under `util/trace/` and
> `util/xs_scripts/trace/`.

---

## Overview

These scripts form the repo's standard workflow for trace triage:

- reproduce or rerun a failing trace workload
- extract debug-window events from gem5 logs
- align BUILD and BIND metadata by `sn`
- summarize `Trace stream PC mismatch` failures
- dump raw trace records around the offending index

Use this spec when changing trace debug scripts, touching trace log formats, or
debugging `src/cpu/o3/trace/` behavior through existing tooling.

---

## Scenario: Trace Debug Tooling

### 1. Scope / Trigger

Read this spec before changing:

- `util/trace/*.py`
- `util/xs_scripts/trace/*`
- `util/xs_scripts/extract_debug_events.sh`
- `src/cpu/o3/trace/*.cc`
- `src/cpu/o3/trace/*.hh`

This spec is mandatory when the task involves one or more of:

- `Trace stream PC mismatch` triage
- trace BUILD/BIND alignment
- wrong-path trace debug windows
- rerunning aborted workloads with debug flags
- changing log strings or event regexes consumed by these scripts

This spec does not define distributed regression scheduling or batch throughput
policy. Keep this file focused on debug and triage contracts.

### 2. Signatures

#### Reproduce a single trace locally

```bash
bash util/xs_scripts/trace/run_trace_champsim.sh [OPTIONS] <trace_file>
```

Supported inputs:

- `-n`, `--maxinsts`, `--max-insts <N>`
- `-f`, `--format <champsim|cbp2025>`

Recognized environment:

- `OUTDIR`
- `XS_MAX_INSTS`
- `TRACE_FORMAT`
- `XS_DEBUG_FLAGS`
- `XS_DEBUG_START`
- `XS_DEBUG_END`
- `XS_WARMUP_INSTS_NO_SWITCH`

Stable behavior:

- always invokes `configs/example/kmhv3.py`
- always enables `--enable-trace-mode`
- always enables `--trace-enable-decoupled-bp`
- writes output under `OUTDIR` or the current working directory

#### Dump trace records around an index

```bash
python3 util/trace/dump_champsim_trace.py --trace <path> [OPTIONS]
```

Common options:

- `--format <champsim|cbp2025>`
- `--start-index <N>`
- `--count <N>`
- `--show-mem`
- `--json`
- `--map-mode <raw|linear|hash>`
- `--addr-base <hex>`
- `--addr-size <hex>`
- `--page-align`

Stable behavior:

- supports raw, `.gz`, and `.xz` trace inputs
- uses ChampSim and CBP2025 layouts matching `src/cpu/o3/trace/*Reader.*`

#### Extract trace-focused events from a gem5 debug log

```bash
python3 util/trace/extract_trace_events.py <log> --from-tick <tick> [OPTIONS]
```

Default event labels:

- `BUILD`
- `SQUASH`
- `COMMIT`
- `BIND`
- `WP_ENTER`
- `WP_EXIT`

Common options:

- `--no-build`
- `--no-squash`
- `--no-commit`
- `--no-bind`
- `--no-wp-enter`
- `--no-wp-exit`
- `--build-pattern <regex>`
- `--squash-pattern <regex>`
- `--commit-pattern <regex>`
- `--bind-pattern <regex>`
- `--wp-enter-pattern <regex>`
- `--wp-exit-pattern <regex>`
- `--limit <N>`
- `--debug`

#### Align BUILD and BIND events

```bash
python3 util/trace/align_trace_bind_events.py <events.txt>
```

Stable output header:

```text
sn	build_tick	build_pc	bind_tick	trace_pc	taken	tracesn
```

#### Extract a generic final-window event trace for every debug run

```bash
bash util/xs_scripts/extract_debug_events.sh <ROOT_DIR>
```

Stable behavior:

- scans every `debug/` directory below `ROOT_DIR`
- writes `<debug>/events.txt`
- derives `from_tick` from `Exiting @ tick`, then `Program aborted at tick`,
  then the last numeric `tick` line

#### Extract panic-window events for PC mismatch failures

```bash
bash util/xs_scripts/trace/extract_panic_events.sh <WORK_ROOT> [WINDOW]
```

Stable behavior:

- scans every `debug/log.txt` below `WORK_ROOT`
- only processes logs containing `Trace stream PC mismatch`
- writes:
  - `<debug>/events_panic.txt`
  - `<debug>/bind_align_panic.tsv`

#### Summarize PC mismatch failures and dump trace snippets

```bash
bash util/xs_scripts/trace/report_pc_mismatch_bind.sh <WORK_ROOT> [OUT_FILE]
```

Stable behavior:

- parses panic metadata from each workload's top-level `log.txt`
- prefers `<workload>/debug/bind_align_panic.tsv`
- falls back to `<workload>/debug/bind_align.tsv`
- emits a TSV summary to stdout or `OUT_FILE`
- optionally writes `<workload>/panic_trace_snippet.txt`

#### Rerun a single aborted workload with a debug window

```bash
python3 util/xs_scripts/trace/rerun_aborted_with_debug.py --work-dir <dir> [OPTIONS]
```

Common options:

- `--arch-script <path>`
- `--debug-flags <flags>`
- `--trace-format <champsim|cbp2025>`
- `--clear-old`
- `--allow-missing-abort`

Stable behavior:

- reads `<work-dir>/log.txt`
- requires `<work-dir>/abort` unless `--allow-missing-abort` is set
- writes rerun outputs to `<work-dir>/debug/`

#### Rerun aborted workloads across multiple hosts

```bash
python3 util/xs_scripts/trace/distributed_rerun_aborted_with_debug.py \
  --work-root <dir> --server-list <file> [OPTIONS]
```

Use this for batch roots, not single-workload directories.

#### Deprecated wrapper

```bash
bash util/xs_scripts/rerun_aborted_with_debug.sh
```

This wrapper is deprecated and intentionally exits with status `1`. Do not wire
new flows to it.

### 3. Contracts

#### Directory contract

A trace workload directory is expected to follow this shape:

```text
<work-dir>/
  log.txt
  abort                  # present for aborted top-level runs
  debug/
    log.txt              # rerun output
    running|completed|abort
    exit_code            # only on failed rerun
    events.txt
    events_panic.txt
    bind_align.tsv
    bind_align_panic.tsv
```

Not every file is mandatory for every workflow, but new tooling should preserve
these names unless there is a migration plan.

#### Log-string contract

These scripts depend on stable log strings:

- `Trace file: <path>`
- `Trace format: <fmt>`
- `Max instructions: <N>`
- `Program aborted at tick <tick>`
- `Exiting @ tick <tick>`
- `Trace stream PC mismatch: built=<pc> expect=<pc> (sn:<sn>)`
- `Bind trace metadata to [sn:<sn>]->[tracesn:<tracesn>]: pc=<pc>, taken=<0|1>`

If runtime logging changes, update both the parsing scripts and this spec in the
same change.

#### Event extraction contract

`extract_trace_events.py` only considers lines that begin with `<tick>:`. The
default regexes are part of the contract for:

- instruction creation
- squash
- commit
- trace metadata binding
- wrong-path enter and exit

If a new log format is introduced, prefer extending the script with new
patterns or switches rather than silently changing the default meaning of an
existing label.

#### Alignment contract

`align_trace_bind_events.py` expects the output of
`extract_trace_events.py`. For each `sn`:

- BUILD keeps the earliest observed tick
- BIND keeps the earliest observed tick
- `build_pc` uses the pre-PC from `Instruction PC (old=>new)`
- missing BUILD or BIND data is printed as `-`

This script is for gem5 event alignment only; it does not reopen the raw trace.

#### Reporting contract

`report_pc_mismatch_bind.sh` mixes data from two locations:

- top-level `<work-dir>/log.txt` for panic metadata
- `<work-dir>/debug/*.tsv` for aligned BUILD/BIND context

When both `bind_align_panic.tsv` and `bind_align.tsv` exist, the panic-window
file wins. Raw trace dumping follows this order:

1. use `tracesn` if present and numeric
2. otherwise fall back to panic `sn`
3. dump a 100-record window ending at that index

#### Rerun contract

`rerun_aborted_with_debug.py` and
`distributed_rerun_aborted_with_debug.py` derive the debug window from the
original top-level `log.txt`:

- default start tick: `max(abort_tick - 1_000_000, 0)`
- default end tick: `abort_tick + 1000`
- if `--debug-start=... --debug-end=...` is already present in the log, reuse
  that suggested window

The rerun helpers set these environment variables for
`run_trace_champsim.sh`:

- `XS_MAX_INSTS`
- `XS_DEBUG_FLAGS`
- `XS_DEBUG_START`
- `XS_DEBUG_END`
- `TRACE_FORMAT`

#### Recommended workflow

Use the narrowest path that answers the debugging question:

1. Reproduce a single trace with `run_trace_champsim.sh` when no batch result
   exists yet.
2. For an aborted workload, rerun it with
   `util/xs_scripts/trace/rerun_aborted_with_debug.py`.
3. For batch roots with many aborted tasks, use
   `distributed_rerun_aborted_with_debug.py`.
4. Extract event windows with `extract_debug_events.sh` or
   `extract_panic_events.sh`.
5. Align BUILD/BIND state with `align_trace_bind_events.py`.
6. Summarize mismatch workloads with `report_pc_mismatch_bind.sh`.
7. Inspect raw trace records with `dump_champsim_trace.py`.

### 4. Validation & Error Matrix

| Surface | Expected validation | Failure behavior |
|---------|---------------------|------------------|
| `run_trace_champsim.sh` | trace file exists; format is `champsim` or `cbp2025` by caller convention | exits non-zero if no trace file or multiple inputs are provided |
| `dump_champsim_trace.py` | trace path exists and format matches file contents | stops at short read; may fail if external `xz` is unavailable for `.xz` fallback |
| `extract_trace_events.py` | log contains `<tick>:` lines after `--from-tick` | produces empty output; `--debug` explains whether tick parsing or range filtering removed everything |
| `align_trace_bind_events.py` | input file comes from `extract_trace_events.py` | unmatched rows remain with `-`; malformed lines are ignored |
| `extract_debug_events.sh` | each `debug/log.txt` contains a recoverable final tick | warns and skips debug dirs without a numeric final tick |
| `extract_panic_events.sh` | `debug/log.txt` contains both mismatch panic and abort tick | warns and skips dirs missing the panic string or abort tick |
| `report_pc_mismatch_bind.sh` | top-level `log.txt` contains panic metadata; dump helper exists | warns on missing trace file or missing `sn`; still emits summary rows when possible |
| `rerun_aborted_with_debug.py` | top-level `log.txt` exists; `abort` exists unless override is set | exits non-zero if parsing fails, trace file is missing, or rerun exits non-zero |
| `distributed_rerun_aborted_with_debug.py` | server list is reachable via SSH and each task dir has parseable abort info | warns per host/task and leaves incomplete reruns in non-completed state |
| deprecated wrapper | none | always exits `1` and prints the replacement entry point |

### 5. Good / Base / Bad Cases

#### Good

You have a batch root with rerun logs under `<workload>/debug/` and want to
triage only PC mismatch failures:

```bash
bash util/xs_scripts/trace/extract_panic_events.sh "$WORK_ROOT" 1000000
bash util/xs_scripts/trace/report_pc_mismatch_bind.sh "$WORK_ROOT" panic_bind.tsv
```

Expected artifacts:

- `debug/events_panic.txt`
- `debug/bind_align_panic.tsv`
- `panic_bind.tsv`
- `panic_trace_snippet.txt` for workloads with a resolvable trace index

#### Base

You only need the final few ticks of every rerun, not panic-specific analysis:

```bash
bash util/xs_scripts/extract_debug_events.sh "$WORK_ROOT"
```

Expected artifact:

- `debug/events.txt`

#### Bad

You run `report_pc_mismatch_bind.sh` before generating any aligned debug events
and then assume missing BUILD/BIND columns mean the runtime never produced them.

This is wrong. Missing alignment data can simply mean no `debug/bind_align*.tsv`
was generated yet.

### 6. Tests Required

When changing these scripts or the log strings they consume, run the narrowest
validation that proves the contract still holds:

1. `--help` or argument-parsing smoke tests for the touched CLI.
2. One representative log extraction check:

   ```bash
   python3 util/trace/extract_trace_events.py <debug-log> --from-tick <tick> --debug | head
   ```

3. One representative alignment check:

   ```bash
   python3 util/trace/align_trace_bind_events.py <events.txt> | head
   ```

4. If rerun helpers changed, run at least one single-workload rerun against an
   existing aborted directory and verify `debug/running`, `debug/completed` or
   `debug/abort`, and `debug/log.txt`.
5. If panic summary logic changed, run:

   ```bash
   bash util/xs_scripts/trace/extract_panic_events.sh <WORK_ROOT>
   bash util/xs_scripts/trace/report_pc_mismatch_bind.sh <WORK_ROOT>
   ```

6. If runtime log strings changed in `src/cpu/o3/trace/`, update the scripts,
   this spec, and the nearby user docs such as `src/cpu/o3/trace/TRACE_USAGE.md`
   in the same review.

### 7. Wrong vs Correct

#### Wrong: use the deprecated wrapper as a real entry point

```bash
bash util/xs_scripts/rerun_aborted_with_debug.sh
```

Why it is wrong:

- it is intentionally a hard failure path
- it does not perform any rerun
- it tells the caller to use the trace-scoped helpers instead

#### Correct: pick the rerun entry that matches the workload scope

```bash
# Single workload
python3 util/xs_scripts/trace/rerun_aborted_with_debug.py --work-dir <work-dir>

# Batch root
python3 util/xs_scripts/trace/distributed_rerun_aborted_with_debug.py \
  --work-root <work-root> --server-list servers.txt
```

#### Wrong: treat top-level and debug logs as interchangeable

```bash
python3 util/trace/extract_trace_events.py <work-dir>/log.txt --from-tick 0
```

Why it is wrong:

- top-level `log.txt` often lacks the debug-window verbosity needed for BUILD,
  BIND, and wrong-path events
- panic summaries read top-level metadata, but event extraction is expected to
  use `debug/log.txt`

#### Correct: use each log for the layer it owns

```bash
# Metadata source
grep -m1 'Trace stream PC mismatch' <work-dir>/log.txt

# Event source
python3 util/trace/extract_trace_events.py <work-dir>/debug/log.txt --from-tick <tick>
```

---

## Related

- `src/cpu/o3/trace/TRACE_USAGE.md`
- `.trellis/spec/backend/logging-guidelines.md`
- `.trellis/spec/backend/error-handling.md`
