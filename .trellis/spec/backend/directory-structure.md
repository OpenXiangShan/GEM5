# Directory Structure

> How backend code is organized in this project.

---

## Overview

XS-GEM5 is organized by simulator subsystem, not by service/controller layers.
The primary split is:

- `src/` for C++ runtime logic and SimObject declarations
- `configs/` for runnable configuration entrypoints and shared option wiring
- `util/` for offline tooling, hooks, and batch scripts
- `tests/` and local `test/` directories for regression coverage

When adding code, put it next to the owning subsystem instead of creating a new
generic top-level bucket.

---

## Directory Layout

```
src/
├── cpu/
│   ├── o3/                # O3 pipeline implementation
│   ├── o3/trace/          # Trace-driven fetch/readers + local docs/tests
│   └── pred/              # Branch predictor implementations and params
├── mem/                   # Cache, memory system, prefetch logic
└── sim/                   # Simulator-wide services and SimObjects

configs/
├── common/                # Shared CLI/options and reusable config helpers
└── example/               # User-facing runnable entry scripts

tests/                     # System-level regression suites
util/                      # Offline tools, style hooks, helper scripts
.trellis/spec/backend/     # AI-facing backend conventions
```

---

## Module Organization

1. Keep C++ headers and implementations in the same subsystem directory.
   Examples:
   - `src/cpu/o3/trace/TraceFetch.hh` + `src/cpu/o3/trace/TraceFetch.cc`
   - `src/sim/arch_db.hh` + `src/sim/arch_db.cc`

2. Keep Python SimObject/config declarations close to the owning subsystem,
   then wire runnable options in `configs/`.
   Examples:
   - `src/sim/ArchDBer.py`
   - `src/cpu/o3/BaseO3CPU.py`
   - `src/cpu/pred/BranchPredictor.py`
   - `configs/common/Options.py`
   - `configs/common/xiangshan.py`

3. Keep subsystem documentation close to the code it explains.
   Examples:
   - `src/cpu/o3/trace/README.md`
   - `src/cpu/o3/trace/TRACE_USAGE.md`
   - `src/cpu/o3/trace/CLAUDE.md`

4. Colocate unit tests with the subsystem when possible.
   Examples:
   - `src/cpu/o3/trace/ChampSimTraceReader.test.cc`
   - `src/cpu/pred/btb/test/`

5. Put reusable command-line tooling under `util/`, not inside runtime source
   directories.
   Examples:
   - `util/arch_db/`
   - `util/git-pre-commit.py`
   - `util/git-commit-msg.py`

---

## Naming Conventions

- C++ implementation files use `.cc`; headers use `.hh`.
- Runtime class/file names typically follow subsystem vocabulary in PascalCase
  or CamelCase.
  Examples:
  - `TraceFetch.cc`
  - `CBP2025TraceReader.hh`
  - `ArchDBer.py`
- Test files usually use `*.test.cc` or a local `test/` directory.
  Examples:
  - `ChampSimTraceReader.test.cc`
  - `fetch_target_queue.test.cc`
- Build glue lives in `SConscript` files inside the owning directory.
- Runnable config scripts are short, lower-case entrypoints.
  Examples:
  - `configs/example/fs.py`
  - `configs/example/kmhv3.py`

Follow the nearest existing directory's style before introducing a new naming
pattern.

---

## Examples

### Example 1: Trace infrastructure

`src/cpu/o3/trace/` is the clearest example of a self-contained subsystem:

- runtime classes: `TraceFetch.cc`, `TraceReader.cc`,
  `ChampSimTraceReader.cc`, `CBP2025TraceReader.cc`
- tests: `ChampSimTraceReader.test.cc`
- docs: `README.md`, `TRACE_USAGE.md`, `TRACE_REVIEW.md`, `CLAUDE.md`

### Example 2: SimObject runtime + Python declaration

`src/sim/arch_db.cc` and `src/sim/ArchDBer.py` show the standard pairing:

- C++ owns runtime behavior
- Python exposes constructor parameters and config surface
- config scripts wire the object into runnable systems

### Example 3: Predictor-local unit tests

`src/cpu/pred/btb/test/` keeps GTest sources, local test doubles, README, and
`SConscript` in the same directory as the predictor subsystem they validate.

---

## Common Mistakes

- Do not put simulator behavior only in `configs/` when the feature is really a
  runtime concern; the implementation belongs in `src/`.
- Do not create a new helper directory when an existing subsystem already owns
  the logic.
- Do not land a new runtime feature without also wiring build rules, config
  surface, and nearby tests/docs.
