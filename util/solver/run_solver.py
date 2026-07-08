#!/usr/bin/env python3
from __future__ import annotations

import argparse
from math import prod
from pathlib import Path
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.solver.executor.ci_local import CiLocalParallelExecutor
from util.solver.parser.bind_targets import bind_problem_targets
from util.solver.parser.load_spec import parse_problem
from util.solver.processing.aggregate import best_trial, evaluate_trial
from util.solver.processing.persist import persist_run_state, write_json
from util.solver.reporting.charts import render_charts
from util.solver.reporting.markdown import render_summary, write_summary
from util.solver.solver.grid import GridSolver
from util.solver.solver.random import RandomSolver


def build_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--problem-ref", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--benchmark-type", default="")
    parser.add_argument(
        "--solver-kind",
        choices=["auto", "grid", "random"],
        default="auto",
    )
    parser.add_argument("--max-parallel-trials", type=int, default=4)
    parser.add_argument("--max-parallel-workloads", type=int, default=1)
    parser.add_argument("--max-trials", type=int)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--gem5-build-type", default="fast")
    parser.add_argument("--timeout-minutes", type=int, default=360)
    parser.add_argument("--specific-benchmarks", default="")
    parser.add_argument("--custom-bin", default="")
    parser.add_argument("--extra-args", default="")
    parser.add_argument("--dry-run", action="store_true")
    return parser


def apply_runtime_overrides(problem, args):
    if args.max_trials is not None:
        problem.stop.max_trials = args.max_trials
    if args.benchmark_type:
        problem.benchmark_type = args.benchmark_type
    if args.custom_bin:
        problem.custom_bin = args.custom_bin
    if problem.custom_bin and args.specific_benchmarks:
        raise ValueError("--custom-bin cannot be combined with --specific-benchmarks")
    if args.specific_benchmarks:
        problem.specific_benchmarks = args.specific_benchmarks
    if problem.custom_bin:
        problem.specific_benchmarks = ""
    if args.extra_args:
        merged = " ".join(part for part in [problem.extra_args.strip(), args.extra_args.strip()] if part)
        problem.extra_args = merged
    return problem


def choose_solver(problem, solver_kind: str, seed: int):
    if solver_kind == "grid":
        return GridSolver(problem)
    if solver_kind == "random":
        return RandomSolver(problem, seed=seed)
    if problem.solver_hint == "grid":
        return GridSolver(problem)
    if problem.solver_hint == "random":
        return RandomSolver(problem, seed=seed)
    total_points = prod(parameter.domain.cardinality() for parameter in problem.parameters)
    max_trials = problem.stop.max_trials
    if max_trials is not None and total_points <= max_trials:
        return GridSolver(problem)
    return RandomSolver(problem, seed=seed)


def _no_improve_count(problem, history) -> int:
    best = None
    stale = 0
    for trial in history:
        if trial.status != "valid" or trial.objective_value is None:
            stale += 1
            continue
        if best is None:
            best = trial.objective_value
            stale = 0
            continue
        improved = (
            trial.objective_value > best
            if problem.objective.direction == "max"
            else trial.objective_value < best
        )
        if improved:
            best = trial.objective_value
            stale = 0
        else:
            stale += 1
    return stale


def should_stop(problem, history, start_time: float) -> tuple[bool, str | None]:
    if problem.stop.max_trials is not None and len(history) >= problem.stop.max_trials:
        return True, "max_trials reached"
    if problem.stop.timeout_hours is not None:
        elapsed_hours = (time.monotonic() - start_time) / 3600.0
        if elapsed_hours >= problem.stop.timeout_hours:
            return True, "timeout_hours reached"
    if problem.stop.no_improve_trials is not None:
        if _no_improve_count(problem, history) >= problem.stop.no_improve_trials:
            return True, "no_improve_trials reached"
    return False, None


def main() -> int:
    args = build_argparser().parse_args()
    workdir = Path(args.workdir).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    problem = parse_problem(args.problem_ref)
    problem = apply_runtime_overrides(problem, args)

    executor = CiLocalParallelExecutor(
        workdir=workdir,
        build_type=args.gem5_build_type,
        max_parallel_trials=args.max_parallel_trials,
        max_parallel_workloads=args.max_parallel_workloads,
        timeout_minutes=args.timeout_minutes,
    )
    metadata = {
        "problem_ref": args.problem_ref,
        "resolved_problem_ref": problem.problem_ref,
        "solver_kind": args.solver_kind,
        "max_parallel_trials": args.max_parallel_trials,
        "max_parallel_workloads": args.max_parallel_workloads,
        "gem5_build_type": args.gem5_build_type,
        "benchmark_type": problem.benchmark_type,
        "specific_benchmarks": problem.specific_benchmarks,
        "custom_bin": problem.custom_bin,
        "extra_args": problem.extra_args,
        "dry_run": args.dry_run,
        "stop_reason": None,
    }
    write_json(workdir / "metadata.json", metadata)

    bind_output = workdir / "binding.json"
    problem = bind_problem_targets(executor, problem, bind_output)
    write_json(workdir / "parsed_problem.json", problem)

    solver = choose_solver(problem, args.solver_kind, args.seed)
    preview = solver.propose([], min(args.max_parallel_trials, problem.stop.max_trials or args.max_parallel_trials))
    if args.dry_run:
        write_json(workdir / "preview_trials.json", preview)
        return 0

    history = []
    solver = choose_solver(problem, args.solver_kind, args.seed)
    start_time = time.monotonic()
    while True:
        stop, reason = should_stop(problem, history, start_time)
        if stop:
            metadata["stop_reason"] = reason
            break
        remaining = None
        if problem.stop.max_trials is not None:
            remaining = problem.stop.max_trials - len(history)
            if remaining <= 0:
                break
        batch_size = args.max_parallel_trials if remaining is None else min(args.max_parallel_trials, remaining)
        trials = solver.propose(history, batch_size)
        if not trials:
            metadata["stop_reason"] = "solver exhausted search space"
            break
        executed = executor.run_trials(problem, trials)
        evaluated = [evaluate_trial(problem, result) for result in executed]
        history.extend(evaluated)
        best = best_trial(history, direction=problem.objective.direction)
        persist_run_state(workdir, problem, history, best)
        summary = render_summary(problem, history)
        write_summary(workdir / "summary.md", summary)
        render_charts(problem, history, workdir / "charts")

    best = best_trial(history, direction=problem.objective.direction)
    persist_run_state(workdir, problem, history, best)
    summary = render_summary(problem, history)
    write_summary(workdir / "summary.md", summary)
    render_charts(problem, history, workdir / "charts")
    write_json(workdir / "metadata.json", metadata)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
