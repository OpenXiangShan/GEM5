# Logging Guidelines

> How logging is done in this project.

---

## Overview

XS-GEM5 uses gem5's built-in debug and message macros. There is no structured
JSON logger and no external logging framework in normal backend code.

The primary tools are:

- `DPRINTF(DebugFlag, ...)` for opt-in verbose tracing
- `inform(...)` for high-signal lifecycle/status messages
- `warn(...)` for recoverable issues
- `fatal(...)` / `panic(...)` for terminating failures

Debug output is expected to be controllable by debug flags, not by ad hoc
boolean prints.

---

## Log Levels

### `DPRINTF(...)`

Use for detailed state transitions, per-cycle flow, predictor internals, and
other potentially high-volume diagnostics.

Examples:

- `src/cpu/o3/trace/TraceFetch.cc`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/o3/fetch.cc`

### `inform(...)`

Use for infrequent setup/lifecycle messages that are useful without enabling a
debug flag.

Example:

- `src/sim/arch_db.cc` announces table creation

### `warn(...)`

Use when execution can continue, but the condition deserves operator attention.

Examples:

- failed trace reader initialization in `src/cpu/o3/trace/TraceFetch.cc`
- unknown DB switches in `src/cpu/pred/btb/decoupled_bpred.cc`
- DB save notice in `src/sim/arch_db.cc`

### `fatal(...)` / `panic(...)`

These terminate the run. They are not routine logging levels; use them only for
invalid configurations or broken invariants.

---

## Structured Logging

There is no structured logging schema. Instead, keep messages stable and
diagnostic by including the most relevant runtime fields inline.

Preferred context fields:

- PC / address / address range
- tick / seqNum / stream id
- component name or debug flag domain
- mode/config value that caused the branch

Examples:

- trace mode startup summary in `src/cpu/o3/trace/TraceFetch.cc`
- predictor stream/PC logging in `src/cpu/pred/btb/decoupled_bpred.cc`
- SQL/table reporting in `src/sim/arch_db.cc`

---

## What to Log

Log:

- feature enablement and important mode summaries during setup
- recoverable but suspicious runtime behavior
- invariant failures with concrete offending values
- opt-in detailed pipeline/predictor transitions behind debug flags

The trace subsystem README documents common debug flag sets:

- `Fetch`
- `TraceReader`
- `O3CPU`
- `BPred`
- `DecoupleBP`

---

## What NOT to Log

- Do not print per-cycle or per-instruction spam through `warn(...)` or
  `inform(...)`; use `DPRINTF(...)`.
- Do not duplicate the same hot-path message on every cycle when a debug flag
  already exists for that domain.
- Do not add host-specific secrets, tokens, or unrelated environment details to
  logs or panic strings.
- Do not use `printf` for long-term runtime diagnostics unless you are matching
  an existing localized convention; prefer gem5 logging/debug macros.

---

## Examples

### Example 1: Trace initialization summary

`src/cpu/o3/trace/TraceFetch.cc` logs trace file and format with `DPRINTF(Fetch, ...)`
at startup.

### Example 2: Predictor pipeline tracing

`src/cpu/pred/btb/decoupled_bpred.cc` uses `DPRINTF(Override, ...)` and
`DPRINTF(DecoupleBP, ...)` for detailed state-machine transitions.

### Example 3: DB lifecycle messages

`src/sim/arch_db.cc` uses `inform(...)` for table creation and `warn(...)` when
backing up the in-memory DB to disk.
