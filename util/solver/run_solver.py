#!/usr/bin/env python3
from __future__ import annotations

import argparse
from math import prod
import os
from pathlib import Path
import signal
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.solver.executor.ci_local import CiLocalParallelExecutor
from util.solver.parser.bind_targets import bind_problem_targets
from util.solver.parser.load_spec import parse_problem
from util.solver.processing.aggregate import (
    best_trial,
    evaluate_trial,
    pareto_frontier,
)
from util.solver.processing.persist import persist_run_state, write_json
from util.solver.reporting.charts import render_charts
from util.solver.reporting.markdown import (
    builtin_report_sections,
    publish_step_summary,
    render_summary,
    write_summary,
)
from util.solver.solver.bayes import BayesSolver
from util.solver.solver.ga import GaSolver
from util.solver.solver.grid import GridSolver
from util.solver.solver.nsga2 import Nsga2Solver
from util.solver.solver.random import RandomSolver
from util.solver.types import CUSTOM_BIN_BENCHMARK_TYPE


def _escape_github_annotation(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def github_annotations_enabled() -> bool:
    return os.environ.get("SOLVER_GITHUB_ANNOTATIONS", "").lower() in {
        "1",
        "true",
        "yes",
        "on",
    }


def format_best_trial(best) -> str:
    if best is None:
        return "none"
    if best.objective_values:
        parts = []
        for key, value in sorted(best.objective_values.items()):
            if value is None:
                continue
            parts.append(f"{key}={value:.6f}")
        if parts:
            return f"{best.trial_id}[{', '.join(parts)}]"
    if best.objective_value is None:
        return "none"
    return f"{best.trial_id}={best.objective_value:.6f}"


def format_evaluated_trial(trial) -> str:
    parts = [
        f"{trial.trial_id}",
        f"status={trial.status}",
        f"duration={trial.duration_sec:.1f}s",
    ]
    if trial.objective_values:
        objective_parts = []
        for key, value in sorted(trial.objective_values.items()):
            if value is None:
                continue
            objective_parts.append(f"{key}={value:.6f}")
        if objective_parts:
            parts.append(f"objectives=[{', '.join(objective_parts)}]")
    elif trial.objective_value is not None:
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
        if (
            annotate
            and os.environ.get("GITHUB_ACTIONS") == "true"
            and github_annotations_enabled()
        ):
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
    parser.add_argument(
        "--benchmark-type",
        default="",
        help=(
            "Built-in benchmark set to use, or custom_bin to disable built-in "
            "checkpoint groups and consume workloads from --custom-bin."
        ),
    )
    parser.add_argument(
        "--solver-kind",
        choices=["auto", "grid", "random", "bayes", "nsga2", "ga"],
        default="auto",
    )
    parser.add_argument("--max-parallel-trials", type=int, default=4)
    parser.add_argument("--max-parallel-workloads", type=int, default=1)
    parser.add_argument("--max-trials", type=int)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--gem5-build-type", default="fast")
    parser.add_argument("--timeout-minutes", type=int, default=360)
    parser.add_argument(
        "--specific-benchmarks",
        default="",
        help=(
            "Optional filter for built-in benchmark lists. Ignored unless "
            "benchmark_type selects a built-in checkpoint group."
        ),
    )
    parser.add_argument(
        "--custom-bin",
        default="",
        help=(
            "Required only when benchmark_type=custom_bin. Accepts one path "
            "or comma/newline-separated checkpoint/bin paths; ignored otherwise."
        ),
    )
    parser.add_argument("--extra-args", default="")
    parser.add_argument("--dry-run", action="store_true")
    return parser


def apply_runtime_overrides(problem, args):
    runtime_messages = []

    if args.max_trials is not None:
        problem.stop.max_trials = args.max_trials
    if args.benchmark_type:
        problem.benchmark_type = args.benchmark_type
    requested_custom_bin = args.custom_bin.strip()
    requested_filters = args.specific_benchmarks.strip()

    if problem.uses_custom_bin_mode():
        if requested_custom_bin:
            problem.custom_bin = requested_custom_bin
        if requested_filters:
            raise ValueError(
                "--specific-benchmarks is not supported when --benchmark-type=custom_bin"
            )
        if problem.specific_benchmarks:
            runtime_messages.append(
                "benchmark_type=custom_bin ignores specific_benchmarks and uses only custom_bin workloads."
            )
        problem.specific_benchmarks = ""
        if not problem.custom_bin.strip():
            raise ValueError(
                "benchmark_type=custom_bin requires a non-empty custom_bin workload list"
            )
        runtime_messages.append(
            "benchmark_type=custom_bin: built-in checkpoint groups are "
            "disabled; trials will use workloads from custom_bin."
        )
    else:
        if requested_filters:
            problem.specific_benchmarks = requested_filters
        ignored_custom_bin = requested_custom_bin or problem.custom_bin.strip()
        if ignored_custom_bin:
            runtime_messages.append(
                f"benchmark_type={problem.benchmark_type} ignores custom_bin; "
                f"set benchmark_type={CUSTOM_BIN_BENCHMARK_TYPE} to use "
                "custom workloads."
            )
        problem.custom_bin = ""
    if args.extra_args:
        merged = " ".join(part for part in [problem.extra_args.strip(), args.extra_args.strip()] if part)
        problem.extra_args = merged
    if problem.uses_custom_bin_mode() and problem.uses_score_txt():
        raise ValueError(
            "score_txt objective does not support benchmark_type=custom_bin; "
            "use stats objective for standalone workload bins"
        )
    setattr(problem, "_runtime_messages", runtime_messages)
    return problem


def runtime_messages(problem) -> list[str]:
    return list(getattr(problem, "_runtime_messages", []))


def choose_solver(problem, solver_kind: str, seed: int):
    if solver_kind == "grid":
        return GridSolver(problem)
    if solver_kind == "random":
        return RandomSolver(problem, seed=seed)
    if solver_kind == "bayes":
        return BayesSolver(problem, seed=seed)
    if solver_kind == "nsga2":
        return Nsga2Solver(problem, seed=seed)
    if solver_kind == "ga":
        return GaSolver(problem, seed=seed)
    if problem.solver_hint == "grid":
        return GridSolver(problem)
    if problem.solver_hint == "random":
        return RandomSolver(problem, seed=seed)
    if problem.solver_hint == "bayes":
        return BayesSolver(problem, seed=seed)
    if problem.solver_hint == "nsga2":
        return Nsga2Solver(problem, seed=seed)
    if problem.solver_hint == "ga":
        return GaSolver(problem, seed=seed)
    total_points = prod(parameter.domain.cardinality() for parameter in problem.parameters)
    max_trials = problem.stop.max_trials
    if max_trials is not None and total_points <= max_trials:
        return GridSolver(problem)
    if problem.is_multi_objective():
        return Nsga2Solver(problem, seed=seed)
    return GaSolver(problem, seed=seed)


def _no_improve_count(problem, history) -> int:
    objectives = problem.objective_list()
    if len(objectives) <= 1:
        best = None
        stale = 0
        direction = objectives[0].direction if objectives else "max"
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
                if direction == "max"
                else trial.objective_value < best
            )
            if improved:
                best = trial.objective_value
                stale = 0
            else:
                stale += 1
        return stale

    stale = 0
    previous_frontier: list = []
    for index, trial in enumerate(history):
        current_history = history[: index + 1]
        current_frontier = pareto_frontier(current_history, objectives)
        frontier_ids = {item.trial_id for item in current_frontier}
        previous_ids = {item.trial_id for item in previous_frontier}
        improved = frontier_ids != previous_ids and trial.trial_id in frontier_ids
        if improved:
            stale = 0
        else:
            stale += 1
        previous_frontier = current_frontier
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


def _signal_name(signum: int | None) -> str:
    if signum is None:
        return "SIGINT"
    try:
        return signal.Signals(signum).name
    except ValueError:
        return f"signal {signum}"


def finalize_run(
    *,
    workdir: Path,
    problem,
    history,
    solver,
    metadata: dict,
    progress: ProgressReporter,
):
    primary = problem.primary_objective()
    if solver is not None:
        metadata["solver_report"] = solver.report_metadata()
    best = best_trial(
        history,
        direction=primary.direction if primary is not None else "max",
        objective=primary,
        objectives=problem.objective_list(),
    )
    persist_run_state(workdir, problem, history, best)
    chart_paths = render_charts(problem, history, workdir / "charts")
    extra_sections = builtin_report_sections(problem, history)
    summary = render_summary(
        problem,
        history,
        metadata=metadata,
        extra_sections=extra_sections,
    )
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
        "partial_summary": bool(metadata.get("partial_summary")),
        "stop_reason": metadata.get("stop_reason"),
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
    return best


def main() -> int:
    args = build_argparser().parse_args()
    progress = ProgressReporter()
    workdir = Path(args.workdir).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    executor = None
    problem = None
    solver = None
    metadata = None
    history = []
    cancelled = False
    cancel_signal = None

    def request_cancel(signum: int, _frame) -> None:
        nonlocal cancelled, cancel_signal
        signal_name = _signal_name(signum)
        cancel_signal = signal_name
        if not cancelled:
            progress.phase(
                "cancel",
                (
                    f"received {signal_name}; stopping in-flight work and "
                    "finalizing partial results"
                ),
                annotate=True,
            )
        cancelled = True
        if metadata is not None:
            metadata["stop_reason"] = f"cancelled by {signal_name}"
            metadata["partial_summary"] = True
            metadata["cancel_signal"] = signal_name
        if executor is not None:
            executor.cancel()

    previous_handlers = {}
    for signum in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.getsignal(signum)
        signal.signal(signum, request_cancel)

    exit_code = 0
    try:
        progress.phase(
            "setup",
            f"workdir={workdir}; problem_ref={args.problem_ref}",
            annotate=True,
        )
        problem = parse_problem(args.problem_ref)
        problem = apply_runtime_overrides(problem, args)
        messages = runtime_messages(problem)
        parameter_names = ", ".join(parameter.name for parameter in problem.parameters)
        search_space = prod(
            parameter.domain.cardinality() for parameter in problem.parameters
        )
        progress.phase(
            "setup",
            (
                f"problem={problem.name}; benchmark={problem.benchmark_type}; "
                f"objectives={'; '.join(obj.display_name() for obj in problem.objective_list())}; "
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
            "partial_summary": False,
            "cancel_signal": None,
            "runtime_messages": messages,
        }
        write_json(workdir / "metadata.json", metadata)
        progress.phase(
            "setup",
            (
                "runtime parameters: "
                f"solver_kind={metadata['solver_kind']}, "
                f"benchmark_type={metadata['benchmark_type']}, "
                f"specific_benchmarks={metadata['specific_benchmarks'] or '<none>'}, "
                f"custom_bin={metadata['custom_bin'] or '<none>'}, "
                f"extra_args={metadata['extra_args'] or '<none>'}, "
                f"max_parallel_trials={metadata['max_parallel_trials']}, "
                f"max_parallel_workloads={metadata['max_parallel_workloads']}, "
                f"gem5_build_type={metadata['gem5_build_type']}"
            ),
        )
        for message in messages:
            progress.phase("input", message)

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
        metadata["solver_backend"] = solver_name
        metadata["solver_report"] = solver.report_metadata()
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
            if cancelled:
                break
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
            try:
                executed = executor.run_trials(problem, trials)
            except KeyboardInterrupt:
                request_cancel(signal.SIGINT, None)
                executed = []
            evaluated = [evaluate_trial(problem, result) for result in executed]
            history.extend(evaluated)
            metadata["solver_report"] = solver.report_metadata()
            primary = problem.primary_objective()
            best = best_trial(
                history,
                direction=primary.direction if primary is not None else "max",
                objective=primary,
                objectives=problem.objective_list(),
            )
            persist_run_state(workdir, problem, history, best)
            progress.batch_completed(
                batch_index,
                evaluated,
                history,
                best,
                batch_duration_sec=time.monotonic() - batch_start,
            )

        if cancelled:
            metadata["stop_reason"] = f"cancelled by {cancel_signal or 'SIGINT'}"
            metadata["partial_summary"] = True
            metadata["cancel_signal"] = cancel_signal or "SIGINT"
            exit_code = 130

        finalize_run(
            workdir=workdir,
            problem=problem,
            history=history,
            solver=solver,
            metadata=metadata,
            progress=progress,
        )
        return exit_code
    finally:
        for signum, previous in previous_handlers.items():
            signal.signal(signum, previous)
        if executor is not None:
            executor.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
