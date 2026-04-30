# Backend Development Guidelines

> Project-specific backend conventions for XS-GEM5.

---

## Overview

In this repository, "backend" means simulator/runtime code, Python config glue,
offline analysis tooling, and the build/test surfaces that change simulator
behavior.

This is not a web-service repository. Most backend work here is a combination
of:

- C++ simulator logic under `src/`
- Python SimObject/config wiring under `src/` and `configs/`
- Targeted tooling under `util/`
- Unit/system regression coverage under `src/**/test/` and `tests/`

---

## Guidelines Index

| Guide | Description | Status |
|-------|-------------|--------|
| [Directory Structure](./directory-structure.md) | Where C++, Python config, tools, docs, and tests belong | Ready |
| [Database Guidelines](./database-guidelines.md) | SQLite-based tracing/instrumentation conventions | Ready |
| [Control-PC Contracts](./control-pc-contracts.md) | Executable contracts for decoder/fetch/predictor PC semantics and inherited predictor params | Ready |
| [FDIP Guidelines](./fdip-guidelines.md) | Executable contracts for the current FTQ-directed ICache prefetch model, parameters, stats, and cleanup semantics | Ready |
| [Error Handling](./error-handling.md) | Runtime checks, fatal paths, warnings, and assertions | Ready |
| [Quality Guidelines](./quality-guidelines.md) | Build/test expectations, hooks, and review checklist | Ready |
| [Logging Guidelines](./logging-guidelines.md) | Debug flags, `warn`/`inform`, and what to emit | Ready |
| [Trace Debug Tooling](./trace-debug-tooling.md) | Trace triage scripts, rerun helpers, and event-alignment contracts | Ready |

---

## Pre-Development Checklist

Read these before changing code:

- Always read [Directory Structure](./directory-structure.md).
- Always read [Quality Guidelines](./quality-guidelines.md).
- Read [Error Handling](./error-handling.md) for any runtime behavior change.
- Read [Control-PC Contracts](./control-pc-contracts.md) before changing
  RISC-V partial decode, predictor-visible PC semantics, fetch ownership
  handoff, or inherited `TimedBaseBTBPredictor` params.
- Read [FDIP Guidelines](./fdip-guidelines.md) before changing FDIP knobs,
  fetch-side FDIP lifecycle/state, old-path refill drop, recent-unused
  suppression, or FDIP-related stats.
- Read [Logging Guidelines](./logging-guidelines.md) when adding new diagnostics
  or debug output.
- Read [Trace Debug Tooling](./trace-debug-tooling.md) before changing
  `util/trace/`, `util/xs_scripts/trace/`, or trace debug log formats.
- Read [Database Guidelines](./database-guidelines.md) before touching
  `ArchDBer`, predictor DB dumps, or `util/arch_db/`.
- Also read `.trellis/spec/guides/cross-layer-thinking-guide.md` when a change
  spans C++ runtime, Python config, and tests.
- Also read `.trellis/spec/guides/code-reuse-thinking-guide.md` before adding a
  new helper, new constant, or parallel config surface.

---

## Scope Notes

- Prefer documenting the repo's current behavior over idealized abstractions.
- Follow the owning subsystem's local conventions before introducing new
  structure.
- Keep docs in English so they can be reused by both humans and agents.

---

**Language**: All documentation in this directory should remain in English.
