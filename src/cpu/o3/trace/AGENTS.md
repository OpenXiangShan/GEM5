# Repository Guidelines

Contributor guide for the O3 trace subsystem, aligned with the capabilities in `CLAUDE.md`. Keep changes focused, reproducible, and covered by tests.

## Project Structure & Module Organization
- `src/cpu/o3/trace/`: Core classes — `TraceReader`, `TraceInstruction`, `ChampSimTraceReader`, `CBP2025TraceReader`. Address mapping aligns with FS config (maps to 0x80000000+).
- `configs/example/xiangshan.py`: Single entrypoint for trace‑driven (FS) runs.
- Results and tooling: `build/` (binaries), `m5out/` (stats/logs), `tests/` (harness via `tests/main.py`).

## Build, Test, and Development Commands
- Build (RISCV, opt): `scons -j$(nproc) --gold-linker build/RISCV/gem5.opt`
- Run (FS mode): `./build/RISCV/gem5.opt configs/example/xiangshan.py --enable-trace-mode --trace-file=/path/to/trace.gz --trace-format=champsim --maxinsts=100000`
  (SE‑specific script removed; use `xiangshan.py` with `--enable-trace-mode`.)
- Internal trace path: `/nfs/home/share/glr/champsim_traces/` (e.g., `cvp1_public/compute_int_0.gz`)
- Prefer FS mode (`xiangshan.py`) for validation; for fast repro, reduce `--maxinsts`.
- Helpful: add `--debug-flags=Fetch,TraceReader,O3CPU` when diagnosing.
- Unit tests: `scons build/NULL/unittests.opt`; system quick sweep: `cd tests && ./main.py run --length quick --isa RISCV --variant opt -j $(nproc)`.

## Coding Style & Naming Conventions
- C/C++: `.clang-format` (Mozilla style, 4 spaces, width 119). Run `clang-format`/`git clang-format` on touched lines.
- Python: Black (line length 79). Use `snake_case` for functions/modules and `CamelCase` for classes.
- Hooks: `pre-commit install && pre-commit run -a` before pushing.

## Testing Guidelines
- Cover trace readers with targeted unit tests where feasible; otherwise validate via SE/FS runs above.
- Before PRs: run unit tests, then one ChampSim trace in FS or SE mode; attach repro command and key `m5out/` snippets (e.g., IPC, cache stats).
- Keep sample traces external (.gz supported); document how to obtain them.

## Commit & Pull Request Guidelines
- Commits: `area: imperative summary` (e.g., `trace: relax MDQ squash check`, `cpu-o3: FTQ bypass in trace mode`). ≤72‑char subject; body explains rationale, risks, and perf/functional impact.
- PRs: clear description, linked issues, exact run command, expected vs. actual, logs/stats evidence; update docs if behavior or flags change.

## Security & Configuration Tips
- Do not commit traces/checkpoints or secrets. Use external storage and reference paths.
- When touching address mapping or BP controls, sanity‑check with a small trace and `--debug-flags=TraceReader`.
