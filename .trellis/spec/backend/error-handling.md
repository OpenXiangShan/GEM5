# Error Handling

> How errors are handled in this project.

---

## Overview

XS-GEM5 backend code relies on gem5 runtime macros and assertions rather than an
exception hierarchy. The standard choices are:

- `fatal(...)` / `fatal_if(...)` for invalid configuration or unsupported usage
- `warn(...)` for recoverable degradation
- `panic(...)` / `panic_if(...)` for internal invariants that should never fail
- `assert(...)` for local developer invariants

Python config code follows the same model through gem5 helper functions such as
`fatal(...)`.

---

## Error Types

### Configuration or contract violations

Terminate early with `fatal` or `fatal_if` when the simulator cannot continue
correctly.

Examples:

- `src/cpu/o3/trace/TraceFetch.cc`
  - unsupported trace wrong-path mode
  - invalid trace page-table window configuration
- `src/cpu/pred/btb/decoupled_bpred.cc`
  - loop buffer enabled without loop predictor
- `configs/example/apu_se.py`
  - invalid runtime combinations handled with `fatal(...)`

### Recoverable runtime issues

Use `warn(...)` when the run may continue with reduced functionality.

Examples:

- `src/cpu/o3/trace/TraceFetch.cc`
  - trace reader initialization failure returns `false` after warning
- `src/cpu/pred/btb/decoupled_bpred.cc`
  - unknown DB switches are warned and reported
- `src/sim/arch_db.cc`
  - DB save path is announced with `warn(...)` before backup

### Internal invariant failures

Use `panic(...)` for code paths that indicate a bug, corrupted state, or an
unimplemented path that must not be silently tolerated.

Examples:

- `src/cpu/o3/trace/TraceFetch.cc`
  - trace stream mismatch and encoding invariants
- `src/cpu/o3/trace/TraceReader.cc`
  - buffer overflow / seqNum discontinuity checks
- `src/cpu/o3/fetch.cc`
  - FTQ alignment and unsupported policy assertions

---

## Error Handling Patterns

1. Validate constructor/setup parameters as early as possible.
   Example: `TraceFetch::TraceFetch()` and
   `TraceFetch::setupTraceTimingPTW()`.

2. Return a status only when the caller can reasonably decide how to recover.
   Example: `TraceFetch::initializeTraceReader()` returns `false` after a
   warning so the caller can choose the next step.

3. Include enough context in fatal/panic messages to debug the failure.
   Preferred fields:
   - PC / address ranges
   - seqNum / stream id
   - enabled mode / option value

4. Keep checks close to the violated invariant instead of relying on a distant
   top-level handler. This codebase does not centralize failures behind a
   generic exception wrapper.

---

## API Error Responses

This repository is not built around HTTP/API responses.

The equivalent "user-visible" surface is:

- CLI argument validation in Python config scripts
- simulator startup validation in C++
- explicit runtime failure messages printed by gem5 macros

Prefer deterministic termination with a clear message over silent fallback when
simulation correctness would be compromised.

---

## Common Mistakes

- Do not use `warn(...)` for conditions that invalidate simulation correctness;
  those should be `fatal(...)` or `panic(...)`.
- Do not defer obvious parameter validation until deep in the runtime.
- Do not add catch-all exception wrappers just to mimic application-framework
  style; they are not idiomatic in this repository.
- Do not emit vague fatal messages. Include the actual offending value, address,
  or mode.
