#!/usr/bin/env python3
from __future__ import annotations

import argparse
from math import prod
import os
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
from util.solver.reporting.markdown import (
    builtin_report_sections,
    publish_step_summary,
    render_summary,
    write_summary,
)
from util.solver.solver.grid import GridSolver
from util.solver.solver.random import RandomSolver


def _escape_github_annotation(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def format_best_trial(best) -> str:
    if best is None or best.objective_value is None:
        return "none"
    return f"{best.trial_id}={best.objective_value:.6f}"


def format_evaluated_trial(trial) -> str:
    parts = [
        f"{trial.trial_id}",
        f"status={trial.status}",
        f"duration={trial.duration_sec:.1f}s",
    ]
    if trial.objective_value is not None:
        parts.append(f"objective={trial.objective_value:.6f}")
    if trial.invalid_reason:
        parts.append(f"reason={trial.invalid_reason}")
    return ", ".join(parts)


class ProgressReporter:
    def __init__(self, label: str = "solver") -> None:
        self.label = label

    def emit(
        self,
        message: str,
        *,
        annotate: bool = False,
        annotation_title: str | None = None,
    ) -> None:
        timestamp = time.strftime("%H:%M:%S")
        print(f"[{self.label} {timestamp}] {message}", flush=True)
        if annotate and os.environ.get("GITHUB_ACTIONS") == "true":
            title = _escape_github_annotation(annotation_title or self.label)
            body = _escape_github_annotation(message)
            print(f"::notice title={title}::{body}", flush=True)

    def phase(self, phase: str, message: str, *, annotate: bool = False) -> None:
        self.emit(
            f"{phase}: {message}",
            annotate=annotate,
            annotation_title=f"{self.label} {phase}",
        )

    def batch_started(
        self,
        batch_index: int,
        trials,
        completed_trials: int,
        max_trials: int | None,
    ) -> None:
        budget = "unbounded" if max_trials is None else str(max_trials)
        trial_ids = ", ".join(trial.trial_id for trial in trials)
        self.phase(
            "batch",
            (
                f"batch {batch_index} start; launching {len(trials)} trial(s); "
                f"completed={completed_trials}/{budget}; trials=[{trial_ids}]"
            ),
            annotate=True,
        )

    def batch_completed(
        self,
        batch_index: int,
        evaluated,
        history,
        best,
        batch_duration_sec: float,
    ) -> None:
        valid_count = sum(1 for trial in history if trial.status == "valid")
        invalid_count = len(history) - valid_count
        self.phase(
            "batch",
            (
                f"batch {batch_index} complete in {batch_duration_sec:.1f}s; "
                f"history={len(history)} total, {valid_count} valid, "
                f"{invalid_count} invalid; best={format_best_trial(best)}"
            ),
            annotate=True,
        )
        for trial in evaluated:
            self.emit(f"trial result: {format_evaluated_trial(trial)}")


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
    progress = ProgressReporter()
    workdir = Path(args.workdir).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    progress.phase(
        "setup",
        f"workdir={workdir}; problem_ref={args.problem_ref}",
        annotate=True,
    )
    problem = parse_problem(args.problem_ref)
    problem = apply_runtime_overrides(problem, args)
    parameter_names = ", ".join(parameter.name for parameter in problem.parameters)
    search_space = prod(
        parameter.domain.cardinality() for parameter in problem.parameters
    )
    progress.phase(
        "setup",
        (
            f"problem={problem.name}; benchmark={problem.benchmark_type}; "
            f"objective={problem.objective.source_kind}:{problem.objective.metric}; "
            f"parameters={len(problem.parameters)} [{parameter_names}]; "
            f"search_space={search_space}"
        ),
    )

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
    progress.phase("bind", "binding solver parameters to live gem5 objects", annotate=True)
    problem = bind_problem_targets(executor, problem, bind_output)
    write_json(workdir / "parsed_problem.json", problem)
    progress.phase(
        "bind",
        f"binding complete; metadata written to {bind_output.name}",
    )

    solver = choose_solver(problem, args.solver_kind, args.seed)
    solver_name = solver.__class__.__name__
    preview_count = min(
        args.max_parallel_trials,
        problem.stop.max_trials or args.max_parallel_trials,
    )
    preview = solver.propose([], preview_count)
    progress.phase(
        "preview",
        f"solver={solver_name}; prepared {len(preview)} preview trial(s)",
        annotate=True,
    )
    if args.dry_run:
        write_json(workdir / "preview_trials.json", preview)
        progress.phase(
            "final",
            f"dry-run complete; preview written to {workdir / 'preview_trials.json'}",
            annotate=True,
        )
        return 0

    history = []
    solver = choose_solver(problem, args.solver_kind, args.seed)
    progress.phase(
        "search",
        (
            f"starting iterative search with solver={solver.__class__.__name__}; "
            f"max_parallel_trials={args.max_parallel_trials}; "
            f"max_parallel_workloads={args.max_parallel_workloads}"
        ),
        annotate=True,
    )
    start_time = time.monotonic()
    batch_index = 0
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
        batch_index += 1
        progress.batch_started(
            batch_index,
            trials,
            completed_trials=len(history),
            max_trials=problem.stop.max_trials,
        )
        batch_start = time.monotonic()
        executed = executor.run_trials(problem, trials)
        evaluated = [evaluate_trial(problem, result) for result in executed]
        history.extend(evaluated)
        best = best_trial(history, direction=problem.objective.direction)
        persist_run_state(workdir, problem, history, best)
        progress.batch_completed(
            batch_index,
            evaluated,
            history,
            best,
            batch_duration_sec=time.monotonic() - batch_start,
        )

    best = best_trial(history, direction=problem.objective.direction)
    persist_run_state(workdir, problem, history, best)
    chart_paths = render_charts(problem, history, workdir / "charts")
    extra_sections = builtin_report_sections(problem, history)
    summary = render_summary(problem, history, extra_sections=extra_sections)
    write_summary(workdir / "summary.md", summary)
    publish_step_summary(summary)
    write_json(workdir / "metadata.json", metadata)
    artifact_manifest = {
        "summary_md": str(workdir / "summary.md"),
        "metadata_json": str(workdir / "metadata.json"),
        "parsed_problem_json": str(workdir / "parsed_problem.json"),
        "binding_json": str(workdir / "binding.json"),
        "history_jsonl": str(workdir / "history.jsonl"),
        "history_csv": str(workdir / "history.csv"),
        "best_result_json": str(workdir / "best_result.json"),
        "charts": chart_paths,
        "extra_sections_count": len(extra_sections),
    }
    write_json(workdir / "artifact_manifest.json", artifact_manifest)
    valid_count = sum(1 for trial in history if trial.status == "valid")
    invalid_count = len(history) - valid_count
    progress.phase(
        "final",
        (
            f"stop_reason={metadata['stop_reason']}; "
            f"history={len(history)} total, {valid_count} valid, "
            f"{invalid_count} invalid; best={format_best_trial(best)}"
        ),
        annotate=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
