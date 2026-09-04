#!/usr/bin/env python3
"""Offline semantic tests for the ci-param-solver dispatch helper."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace


REPO_ROOT = Path(__file__).resolve().parents[4]
HELPER = Path(__file__).with_name("solver_ci_dispatch.py")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
BOP_SCORE = (
    "configs/solver_specs/l2_vbop_bop_large_nsga2_score_search.py:"
    "L2VbopBopLargeNsga2ScoreSearch"
)
BOP_BIN = (
    "configs/solver_specs/l2_vbop_bop_large_nsga2_bin_search.py:"
    "L2VbopBopLargeNsga2BinSearch"
)


def run_case(name: str, arguments: list[str], expected: int, needle: str) -> None:
    result = subprocess.run(
        [sys.executable, str(HELPER), "validate", *arguments],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
    )
    output = (result.stdout + result.stderr).strip()
    if result.returncode != expected or needle not in output:
        raise AssertionError(
            f"{name}: expected exit={expected} and {needle!r}; "
            f"got exit={result.returncode}\n{output}"
        )
    print(f"PASS {name}: exit={result.returncode}, matched={needle!r}")


def check_bop_constraint() -> None:
    from configs.solver_specs.l2_vbop_bop_large_nsga2_score_search import (
        L2VbopBopLargeNsga2ScoreSearch,
    )

    class Param:
        def convert(self, value):
            return value

    class BopLarge:
        def __init__(self):
            self._params = {"score_max": Param()}
            self.score_max = None

    root = SimpleNamespace(
        system=SimpleNamespace(
            l2_wrappers=[
                SimpleNamespace(prefetcher=SimpleNamespace(bop_large=BopLarge()))
            ]
        )
    )
    trial = SimpleNamespace(round_max=50, score_ratio=70)
    L2VbopBopLargeNsga2ScoreSearch.apply_trial(root, trial)
    actual = root.system.l2_wrappers[0].prefetcher.bop_large.score_max
    if actual != 35:
        raise AssertionError(f"BOP score_max derivation returned {actual}, expected 35")
    print("PASS bop_apply_trial_constraint: 50 * 70 // 100 = 35")


def main() -> int:
    common = [
        "--branch",
        "solver-bop-demo",
        "--configuration",
        "kmhv3.py",
    ]

    run_case(
        "complete_builtin_bop",
        [
            *common,
            "--problem-ref",
            BOP_SCORE,
            "--benchmark-type",
            "spec06-rva23-novec-gcc16-0.3c",
            "--specific-benchmarks",
            "mcf,omnetpp,xalancbmk",
            "--solver-kind",
            "nsga2",
            "--max-parallel-trials",
            "16",
            "--max-parallel-workloads",
            "10",
            "--max-trials",
            "4000",
        ],
        0,
        "7 objective(s)",
    )
    run_case(
        "complete_custom_bop",
        [
            *common,
            "--problem-ref",
            BOP_BIN,
            "--benchmark-type",
            "custom_bin",
            "--custom-bin",
            "/tmp/example.zstd",
            "--solver-kind",
            "nsga2",
            "--max-parallel-trials",
            "32",
            "--max-parallel-workloads",
            "1",
        ],
        0,
        "benchmark_type=custom_bin",
    )
    run_case(
        "reject_custom_score",
        [
            *common,
            "--problem-ref",
            BOP_SCORE,
            "--benchmark-type",
            "custom_bin",
            "--custom-bin",
            "/tmp/example.zstd",
            "--solver-kind",
            "nsga2",
        ],
        2,
        "score_txt objective does not support benchmark_type=custom_bin",
    )
    run_case(
        "reject_multiobjective_bayes",
        [
            *common,
            "--problem-ref",
            BOP_SCORE,
            "--benchmark-type",
            "spec06-rva23-novec-gcc16-0.3c",
            "--solver-kind",
            "bayes",
        ],
        2,
        "supports only single-objective specs",
    )
    run_case(
        "reject_custom_filter",
        [
            *common,
            "--problem-ref",
            BOP_BIN,
            "--benchmark-type",
            "custom_bin",
            "--custom-bin",
            "/tmp/example.zstd",
            "--specific-benchmarks",
            "mcf",
        ],
        2,
        "specific_benchmarks must be empty",
    )
    run_case(
        "reject_zero_parallelism",
        [
            *common,
            "--problem-ref",
            BOP_SCORE,
            "--benchmark-type",
            "spec06-rva23-novec-gcc16-0.3c",
            "--max-parallel-trials",
            "0",
        ],
        2,
        "max_parallel_trials must be > 0",
    )
    run_case(
        "reject_smt",
        [
            *common,
            "--problem-ref",
            BOP_SCORE,
            "--benchmark-type",
            "gcc12-spec06-smt-0.3c",
        ],
        2,
        "SMT benchmark_type",
    )
    check_bop_constraint()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
