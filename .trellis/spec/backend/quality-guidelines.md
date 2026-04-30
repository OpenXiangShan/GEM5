# Quality Guidelines

> Code quality standards for backend development.

---

## Overview

Backend quality in this repository is defined by four things:

- the code builds in the relevant SCons target
- targeted tests or regressions cover the touched subsystem
- style/hooks are respected
- the change matches gem5/XS-GEM5 review conventions

Use the smallest validation set that proves the change is correct, but do not
skip validation entirely.

---

## Forbidden Patterns

- Do not bypass hooks with `--no-verify`.
- Do not disable tests or weaken assertions to hide a bug.
- Do not commit code that does not compile for the affected target.
- Do not delete tracked files directly; move them to `.recycle_bin/` first per
  repo policy.
- Do not introduce broad refactors when a local subsystem fix is sufficient.
- Do not add a new helper or constant before checking whether the repository
  already has one.

---

## Required Patterns

### Build and test the owning subsystem

Examples:

- global unit tests: `scons build/NULL/unittests.opt`
- single unit test target: `scons build/NULL/base/bitunion.test.opt`
- predictor-local tests: `src/cpu/pred/btb/test/SConscript`

### Keep build glue in sync

When a new source or test file is added, update the owning `SConscript`.

Examples:

- `src/cpu/pred/btb/test/SConscript`
- `src/sim/SConscript`
- `src/cpu/pred/SConscript`

### Keep config surface and runtime surface aligned

If a new runtime knob is user-facing, wire it through the Python config layer.

Examples:

- `src/sim/ArchDBer.py` + `configs/common/Options.py`
- `src/cpu/o3/BaseO3CPU.py` + `configs/example/kmhv3.py`

### Follow gem5 commit conventions

Commit headers are validated by `util/git-commit-msg.py` and should use:

- one or more gem5 tags
- `tag1,tag2: Short imperative summary`
- a blank line before the description

See also `CONTRIBUTING.md`.

---

## Testing Requirements

At minimum, run the narrowest relevant validation before review:

1. Build the affected target.
2. Run the affected unit test(s) when they exist.
3. Run targeted system/regression coverage when the change crosses runtime
   boundaries.

Common patterns in this repo:

- GTest-based unit tests for C++ components
  - `src/cpu/o3/trace/ChampSimTraceReader.test.cc`
  - `src/cpu/pred/btb/test/*.test.cc`
- system-level regressions under `tests/`
  - documented in `TESTING.md`
- subsystem-specific execution notes in nearby READMEs
  - `src/cpu/pred/btb/test/README.md`
  - `src/cpu/o3/trace/README.md`

For config-only or CLI-only changes, at least verify the affected script still
parses/builds the intended target.

---

## Code Review Checklist

Reviewers and authors should check:

- Does the code live in the right subsystem directory?
- Are `SConscript`, Python params/options, and docs updated where needed?
- Is error handling using `warn`/`fatal`/`panic` consistently with repo style?
- Are verbose diagnostics behind `DPRINTF(...)` instead of unconditional prints?
- Is there a targeted build/test story for the modified code?
- If a DB/schema changed, were producers, config declarations, and readers all
  updated together?
- If files were removed, were they moved to `.recycle_bin/` instead of deleted?

---

## Examples

### Example 1: Style and staging hooks

`util/git-pre-commit.py` validates staged files and rejects style/encoding
issues. `.pre-commit-config.yaml` adds whitespace, YAML/JSON, and Black checks.

### Example 2: Test documentation

`TESTING.md` documents the expected unit-test and system-test workflow. Treat it
as the default baseline before posting code for review.

### Example 3: Local subsystem test ownership

`src/cpu/pred/btb/test/README.md` and `src/cpu/pred/btb/test/SConscript` show
the preferred pattern for subsystem-owned tests: colocated docs, sources, and
build rules.
