# Database Guidelines

> Database patterns and conventions for this project.

---

## Overview

This repository does not use an ORM or an application-facing migration system.
Database usage is primarily SQLite-based instrumentation for architecture
research, predictor tracing, and offline analysis.

Treat database code here as tracing infrastructure, not business storage:

- runtime producers write trace records during simulation
- schemas are declared explicitly in code/config
- offline scripts read `.db` artifacts after the run

Examples:

- `src/sim/arch_db.cc`
- `src/cpu/pred/general_arch_db.cc`
- `configs/common/xiangshan.py`
- `util/arch_db/*.py`

---

## Query Patterns

### 1. Write to an in-memory SQLite database during simulation

Hot paths stage data in memory first, then flush once at exit:

- `src/sim/arch_db.cc` opens `:memory:` and registers an exit callback to
  `save_db()`
- `src/cpu/pred/general_arch_db.cc` follows the same pattern for predictor DBs

This avoids repeated disk writes in timing-sensitive code.

### 2. Build schema explicitly with SQL strings

There is no schema DSL or ORM model layer. Tables are declared with concrete
`CREATE TABLE ...` strings:

- `configs/common/xiangshan.py`
- `configs/example/fs.py`
- `src/cpu/pred/general_arch_db.cc`

### 3. Use small Python readers for post-processing

Offline scripts connect directly with the standard library `sqlite3` module:

- `util/arch_db/mem_trace.py`
- `util/arch_db/pf_trace.py`
- `util/arch_db/mix_trace.py`

---

## Migrations

There is no migration history or migration runner.

When a schema changes:

1. Update the producer and the schema declaration in the same change.
2. Update the relevant consumer scripts and docs.
3. Regenerate `.db` outputs instead of trying to migrate old files in place.

Typical update sites are:

- CLI/config surface in `configs/common/Options.py`
- table definitions in `configs/common/xiangshan.py` or `configs/example/fs.py`
- writer code in `src/sim/arch_db.cc` or `src/cpu/pred/general_arch_db.cc`
- analysis scripts under `util/arch_db/`

---

## Naming Conventions

- Database file path is exposed as configuration, not hardcoded.
  Examples:
  - `--arch-db-file` in `configs/common/Options.py`
  - `arch_db_file = Param.String(...)` in `src/sim/ArchDBer.py`

- Table names are short domain nouns in CamelCase.
  Examples:
  - `MemTrace`
  - `L1PFTrace`
  - `LifeTimeCommitTrace`
  - `SMSTrainTrace`

- Column names are compact and domain-specific, often using established
  architecture abbreviations.
  Examples:
  - `PC`, `VADDR`, `PADDR`, `PFSrc`
  - `TriggerOffset`, `OldAddr`, `CurAddr`

- Output filenames are short run artifacts rather than long logical names.
  Examples:
  - `bp.db`
  - `mem_trace.db`

---

## Examples

### Example 1: ArchDB runtime

`src/sim/arch_db.cc` creates tables, writes inserts into the in-memory DB, and
backs up to disk during shutdown.

### Example 2: Predictor DB helper

`src/cpu/pred/general_arch_db.cc` abstracts table creation and record writing
for branch-predictor-related traces, but still uses direct SQLite APIs.

### Example 3: Offline analysis

`util/arch_db/README.md` and the scripts in `util/arch_db/` show the intended
workflow: run the simulator, produce `.db`, then analyze offline.

---

## Common Mistakes

- Do not add disk-backed SQLite writes inside hot runtime loops; stage in
  memory and flush once.
- Do not assume existing `.db` files remain compatible after schema changes.
- Do not introduce ORM or migration abstractions; this codebase consistently
  uses explicit SQLite APIs.
- Do not change table definitions without also updating the reader scripts and
  config-layer schema declarations.
