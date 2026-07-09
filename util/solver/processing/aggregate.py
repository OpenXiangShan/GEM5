from __future__ import annotations

from pathlib import Path
from statistics import fmean

from util.solver.processing.extract import (
    collect_workload_stats,
    count_abort_files,
    count_workload_dirs,
    parse_score_file,
)
from util.solver.types import EvaluatedTrial, ObjectiveSpec, ParsedProblem, TrialExecutionResult


def _extract_objective_value(
    objective: ObjectiveSpec,
    execution: TrialExecutionResult,
    spec_dir: Path,
    workload_count: int,
    metrics: dict,
) -> tuple[float | None, str | None]:
    if objective.source_kind == "stats":
        values = collect_workload_stats(spec_dir, objective.metric)
        metrics.setdefault("stats_values", {})[objective.metric] = values
        metrics.setdefault("stats_samples", {})[objective.metric] = len(values)
        if not values:
            return None, f"missing stats metric {objective.metric}"
        if workload_count and len(values) != workload_count:
            return (
                None,
                f"metric {objective.metric} missing in {workload_count - len(values)} workload(s)",
            )
        return fmean(values.values()), None
    if objective.source_kind == "score_txt":
        score_path = execution.raw_files.get("score_txt")
        if not score_path or not Path(score_path).is_file():
            return None, f"missing score.txt metric {objective.metric}"
        score_metrics = metrics.get("score_metrics")
        if score_metrics is None:
            score_metrics = parse_score_file(score_path)
            metrics["score_metrics"] = score_metrics
        value = score_metrics.get(objective.metric)
        if value is None:
            return None, f"missing score metric {objective.metric}"
        return value, None
    return None, f"unsupported objective source {objective.source_kind}"


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
    objective_values: dict[str, float | None] = {}

    if execution.status != "completed":
        invalid_reason = execution.status
    elif execution.error:
        invalid_reason = execution.error
    elif execution.return_code not in (0, None):
        invalid_reason = f"return_code={execution.return_code}"
    elif abort_count > 0:
        invalid_reason = f"{abort_count} abort files"
    else:
        for objective in problem.objective_list():
            value, error = _extract_objective_value(
                objective,
                execution,
                spec_dir,
                workload_count,
                metrics,
            )
            objective_values[objective.key()] = value
            if error is not None:
                invalid_reason = error
                break
        primary = problem.primary_objective()
        if primary is not None:
            objective_value = objective_values.get(primary.key())

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
        objective_values=objective_values,
    )

def objective_value_for_trial(
    trial: EvaluatedTrial,
    objective: ObjectiveSpec,
) -> float | None:
    if trial.objective_values:
        return trial.objective_values.get(objective.key())
    if trial.objective_value is not None:
        return trial.objective_value
    return None


def trial_dominates(
    lhs: EvaluatedTrial,
    rhs: EvaluatedTrial,
    objectives: list[ObjectiveSpec],
) -> bool:
    strictly_better = False
    for objective in objectives:
        lhs_value = objective_value_for_trial(lhs, objective)
        rhs_value = objective_value_for_trial(rhs, objective)
        if lhs_value is None or rhs_value is None:
            return False
        if objective.direction == "max":
            if lhs_value < rhs_value:
                return False
            if lhs_value > rhs_value:
                strictly_better = True
        else:
            if lhs_value > rhs_value:
                return False
            if lhs_value < rhs_value:
                strictly_better = True
    return strictly_better


def pareto_frontier(
    history: list[EvaluatedTrial],
    objectives: list[ObjectiveSpec],
) -> list[EvaluatedTrial]:
    valid_trials = [
        trial
        for trial in history
        if trial.status == "valid"
        and all(objective_value_for_trial(trial, objective) is not None for objective in objectives)
    ]
    frontier = []
    for candidate in valid_trials:
        dominated = False
        for other in valid_trials:
            if other is candidate:
                continue
            if trial_dominates(other, candidate, objectives):
                dominated = True
                break
        if not dominated:
            frontier.append(candidate)
    return sorted(frontier, key=lambda item: (item.generation, item.trial_id))


def crowding_distance(
    trials: list[EvaluatedTrial],
    objectives: list[ObjectiveSpec],
) -> dict[str, float]:
    if not trials:
        return {}
    if len(trials) <= 2:
        return {trial.trial_id: float("inf") for trial in trials}

    distance = {trial.trial_id: 0.0 for trial in trials}
    for objective in objectives:
        ordered = sorted(
            trials,
            key=lambda trial: objective_value_for_trial(trial, objective),
        )
        first = objective_value_for_trial(ordered[0], objective)
        last = objective_value_for_trial(ordered[-1], objective)
        if first is None or last is None:
            continue
        distance[ordered[0].trial_id] = float("inf")
        distance[ordered[-1].trial_id] = float("inf")
        span = last - first
        if span == 0:
            continue
        for index in range(1, len(ordered) - 1):
            current_id = ordered[index].trial_id
            if distance[current_id] == float("inf"):
                continue
            prev_value = objective_value_for_trial(ordered[index - 1], objective)
            next_value = objective_value_for_trial(ordered[index + 1], objective)
            if prev_value is None or next_value is None:
                continue
            distance[current_id] += (next_value - prev_value) / span
    return distance


def best_trial(
    history: list[EvaluatedTrial],
    direction: str = "max",
    objective: ObjectiveSpec | None = None,
    objectives: list[ObjectiveSpec] | None = None,
) -> EvaluatedTrial | None:
    active_objectives = list(objectives or ([] if objective is None else [objective]))
    if len(active_objectives) > 1:
        frontier = pareto_frontier(history, active_objectives)
        return frontier[0] if frontier else None

    valid_trials = [
        trial
        for trial in history
        if trial.status == "valid"
        and (
            (
                objective_value_for_trial(trial, objective) is not None
                if objective is not None
                else trial.objective_value is not None
            )
        )
    ]
    if not valid_trials:
        return None
    reverse = direction == "max"
    if objective is None:
        return sorted(valid_trials, key=lambda item: item.objective_value, reverse=reverse)[0]
    return sorted(
        valid_trials,
        key=lambda item: objective_value_for_trial(item, objective),
        reverse=reverse,
    )[0]
