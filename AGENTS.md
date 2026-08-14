# Repository Guidelines

This file defines the collaboration rules and workflow entry points for this repository.

- For complex tasks, follow the process first instead of jumping straight into code changes.
- For non-trivial behavioral changes, explain the background, assumptions, risks, and validation plan.
- If documentation conflicts, treat the source code as the ground truth, then consult architecture and process docs.

## Planning

For complex or ambiguous tasks, make a concise plan before editing and keep it
updated during the task. Do not create a separate plan file unless explicitly
requested or the work needs to span multiple sessions or contributors.

## Repository Map

Start with these directories first:

- `src/`: core source code (C++ / Python), especially `arch/riscv/`, `cpu/o3/`, and `cpu/pred/`
- `configs/`: runtime configurations, especially `configs/example/kmhv3.py`
- `tests/`: test entry points
- `util/`: helper scripts and tools
- `docs/`: architecture, design, and other project documentation
  - `docs/design-docs/frontend/`: design-oriented Kunminghu v3 frontend/BPU notes; prefer this directory for design motivation, constraints, and tradeoffs

For a higher-level map of the codebase, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Environment Assumptions

This repository is primarily developed on shared Linux servers.

- For full-system, checkpoint, and difftest-related tasks, prefer assuming `GCBV_REF_SO` is available.
- The default CI-style reference path is:
  `GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so`
- Normal single-core GCB checkpoint slices are expected to carry their own restorer.
  Do not require `GCB_RESTORER`; pass `--gcpt-restorer` only when intentionally
  overriding the embedded restorer for legacy checkpoints.
- Whether an external restorer and `AM_HOME` are needed depends on the task:
  - legacy restore override workflows may require checking the relevant restorer variable
  - some frontend micro-tests and bare-metal test flows may require checking `AM_HOME`
- Before running environment-dependent tasks, check the relevant variables instead of assuming local defaults are correct.

## Build, Run, and Test Entry Points

Common entry points:

- Build optimized binary:
  `scons build/RISCV/gem5.opt --gold-linker -j64`
- Build debug binary:
  `scons build/RISCV/gem5.debug --gold-linker -j64 --debug-cycle`
- Run the XiangShan configuration with a normal bin image:
  `./build/RISCV/gem5.opt ./configs/example/kmhv3.py --raw-cpt --generic-rv-cpt=<path>`
- Run the XiangShan configuration with a checkpoint slice (`.zstd` GCPT slice):
  `./build/RISCV/gem5.opt ./configs/example/kmhv3.py --generic-rv-cpt=<path>`
  - do not add `--raw-cpt` for checkpoint slices such as SPEC slice `.zstd` inputs
- SE mode example:
  `./build/RISCV/gem5.opt ./configs/example/se.py -c <binary>`
- Build all unit tests:
  `scons build/RISCV/unittests.opt -j100 --unit-test`

If you need a more systematic understanding of module boundaries, configuration entry points, or execution flow, read [ARCHITECTURE.md](ARCHITECTURE.md) first.

## Style and Naming

- C / C++: follow `.clang-format`
- Python: follow the repository's existing formatting and checking workflow
- Naming:
  - types / classes: UpperCamelCase
  - functions / methods: lower_snake_case
  - constants: ALL_CAPS
- Use English for code comments and commit messages
- Keep changes simple and avoid introducing functionality unrelated to the current task

## Validation Expectations

For non-trivial changes, do not stop at code edits alone. Validation should match the level of risk.

Prefer these principles:

- Behavioral changes: provide a minimal reproduction, key logs, statistics, or test results
- Refactors: confirm there is no behavioral regression, and compare key statistics when needed
- Frontend / BPU / timing-related changes: prefer targeted workloads, unit tests, or checkpoint-based regression
- Analysis tasks: clearly distinguish confirmed facts, current hypotheses, and unresolved questions

If full validation cannot be completed in the current environment, explicitly state the gap and the remaining risk.

## Commit and PR Expectations

- Use imperative English in commit messages
- Prefer module-prefixed commit titles focused on a single change, for example:
  `cpu-o3: Fix tage allocation`
- PRs should explain:
  - motivation
  - approach
  - scope of impact
  - validation method and results
- Run the repository's style checks and required tests before submission

## Related Documents

- [ARCHITECTURE.md](ARCHITECTURE.md): high-level architecture map of the repository
