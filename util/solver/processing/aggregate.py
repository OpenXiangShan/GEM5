from __future__ import annotations

from pathlib import Path
from statistics import fmean

from util.solver.processing.extract import (
    collect_workload_stats,
    count_abort_files,
    count_workload_dirs,
    parse_score_file,
)
from util.solver.types import EvaluatedTrial, ParsedProblem, TrialExecutionResult


def evaluate_trial(problem: ParsedProblem, execution: TrialExecutionResult) -> EvaluatedTrial:
    trial_dir = Path(execution.outdir)
    spec_dir = trial_dir / "raw" / "spec_all"
    abort_count = count_abort_files(spec_dir)
    workload_count = count_workload_dirs(spec_dir)
    metrics = {
        "abort_count": abort_count,
        "workload_count": workload_count,
    }
    invalid_reason = None
    objective_value = None

    if execution.status != "completed":
        invalid_reason = execution.status
    elif execution.return_code not in (0, None):
        invalid_reason = f"return_code={execution.return_code}"
    elif abort_count > 0:
        invalid_reason = f"{abort_count} abort files"
    elif problem.objective.source_kind == "stats":
        values = collect_workload_stats(spec_dir, problem.objective.metric)
        metrics["stats_values"] = values
        metrics["stats_samples"] = len(values)
        if not values:
            invalid_reason = f"missing stats metric {problem.objective.metric}"
        elif workload_count and len(values) != workload_count:
            invalid_reason = (
                f"metric {problem.objective.metric} missing in "
                f"{workload_count - len(values)} workload(s)"
            )
        else:
            objective_value = fmean(values.values())
    elif problem.objective.source_kind == "score_txt":
        score_path = execution.raw_files.get("score_txt")
        if not score_path or not Path(score_path).is_file():
            invalid_reason = f"missing score.txt metric {problem.objective.metric}"
        else:
            score_metrics = parse_score_file(score_path)
            metrics["score_metrics"] = score_metrics
            objective_value = score_metrics.get(problem.objective.metric)
            if objective_value is None:
                invalid_reason = f"missing score metric {problem.objective.metric}"
    else:
        invalid_reason = f"unsupported objective source {problem.objective.source_kind}"

    status = "valid" if invalid_reason is None else "invalid"
    return EvaluatedTrial(
        trial_id=execution.trial_id,
        generation=execution.generation,
        assignments=execution.assignments,
        status=status,
        objective_value=objective_value,
        metrics=metrics,
        invalid_reason=invalid_reason,
        outdir=execution.outdir,
        duration_sec=execution.duration_sec,
        raw_files=execution.raw_files,
    )


def best_trial(history: list[EvaluatedTrial], direction: str = "max") -> EvaluatedTrial | None:
    valid_trials = [
        trial for trial in history
        if trial.status == "valid" and trial.objective_value is not None
    ]
    if not valid_trials:
        return None
    reverse = direction == "max"
    return sorted(valid_trials, key=lambda item: item.objective_value, reverse=reverse)[0]
