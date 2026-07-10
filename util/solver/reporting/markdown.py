from __future__ import annotations

import os
from pathlib import Path

from util.solver.processing.aggregate import (
    best_trial,
    crowding_distance,
    objective_value_for_trial,
    pareto_frontier,
)
from util.solver.reporting.charts import best_objective_series
from util.solver.types import EvaluatedTrial, ObjectiveSpec, ParsedProblem

MERMAID_CONVERGENCE_FULL_X_AXIS_LIMIT = 12


def builtin_report_sections(
    problem: ParsedProblem,
    history: list[EvaluatedTrial],
) -> list[str]:
    sections = []
    if problem.name.startswith("VTAGE"):
        sections.extend(_vtage_report_sections(history))
    return sections


def _format_objective(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.6f}"


def _format_solver_value(value) -> str:
    if isinstance(value, float):
        if value == float("inf"):
            return "inf"
        return f"{value:.6f}"
    return str(value)


def _status_counts(history: list[EvaluatedTrial]) -> dict[str, int]:
    counts = {}
    for trial in history:
        counts[trial.status] = counts.get(trial.status, 0) + 1
    return counts


def _solver_algorithm_section(metadata: dict | None) -> list[str]:
    if not metadata:
        return []
    solver_report = metadata.get("solver_report")
    if not isinstance(solver_report, dict) or not solver_report:
        return []

    preferred_order = [
        "algorithm",
        "solver_backend",
        "generation",
        "population_size",
        "elite_count",
        "tournament_size",
        "base_estimator",
        "acq_func",
        "acq_optimizer",
        "n_initial_points",
        "mutation_prob",
        "crossover_prob",
        "last_generation_mode",
        "last_population_size",
        "last_frontier_size",
        "last_selected_parent_pool",
        "last_elite_count",
        "last_best_objective",
        "last_best_transformed_objective",
        "last_observed_trials",
        "last_model_fit_size",
        "observed_trials",
        "last_mean_objective",
        "last_generated_trials",
        "pending_trials",
        "rng_seen_assignments",
        "next_trial_index",
    ]
    skip_keys = {"generation_history"}

    explanations = {
        "algorithm": "Backend algorithm used for candidate generation.",
        "solver_backend": "Concrete solver implementation class selected by the controller.",
        "generation": "Current generation index already emitted by the solver.",
        "population_size": "Target evolutionary population size used to maintain the parent pool.",
        "elite_count": "Configured number of incumbent elites reserved as guaranteed GA breeding seeds.",
        "tournament_size": "Tournament width used by GA parent selection.",
        "base_estimator": "Surrogate model family used by Bayesian optimization.",
        "acq_func": "Acquisition function used to trade off exploitation and exploration.",
        "acq_optimizer": "How the acquisition function is optimized to propose the next trial.",
        "n_initial_points": "Number of startup random observations before relying on the surrogate model.",
        "mutation_prob": "Approximate fraction of parameters mutated when producing a child.",
        "crossover_prob": "Probability of recombining two parents instead of cloning them.",
        "last_generation_mode": "Whether the latest batch came from initial random sampling or offspring evolution.",
        "last_population_size": (
            "Number of valid historical trials available to the "
            "evolutionary population builder before the latest propose step."
        ),
        "last_frontier_size": (
            "Current Pareto frontier size seen by the solver before "
            "generating the latest batch."
        ),
        "last_selected_parent_pool": (
            "How many individuals were retained as the parent pool "
            "for the latest offspring step."
        ),
        "last_elite_count": (
            "How many incumbent elites were force-kept in the latest "
            "GA parent pool before crossover and mutation."
        ),
        "last_best_objective": (
            "Best valid incumbent objective value visible to GA "
            "before the latest propose step."
        ),
        "last_best_transformed_objective": (
            "Best internal minimization target visible to Bayesian optimization "
            "after max/min direction normalization."
        ),
        "last_observed_trials": (
            "How many newly completed valid trials were incorporated into the "
            "Bayesian optimizer during the latest propose step."
        ),
        "last_model_fit_size": (
            "Total number of valid observations currently informing the "
            "Bayesian surrogate model."
        ),
        "observed_trials": (
            "Cumulative number of valid observations already told back to the "
            "Bayesian optimizer."
        ),
        "last_mean_objective": (
            "Mean valid incumbent objective value visible to GA "
            "before the latest propose step."
        ),
        "last_generated_trials": (
            "How many fresh trials were generated in the latest "
            "propose step before any pending carry-over."
        ),
        "pending_trials": "Generated but not yet dispatched trials still buffered inside the solver.",
        "rng_seen_assignments": (
            "Unique assignments already seen by the solver; useful to "
            "judge search-space coverage and duplication pressure."
        ),
        "next_trial_index": "Next controller-side trial id counter.",
    }

    lines = [
        "## Solver Algorithm",
        "",
        "This section explains how the candidate generator is behaving, not just what objective values came out.",
        "",
    ]
    emitted = set()
    for key in preferred_order:
        if key not in solver_report:
            continue
        emitted.add(key)
        explanation = explanations.get(key, "")
        if explanation:
            lines.append(
                f"- {key}: `{_format_solver_value(solver_report[key])}`. {explanation}"
            )
        else:
            lines.append(f"- {key}: `{_format_solver_value(solver_report[key])}`")
    for key in sorted(solver_report):
        if key in emitted or key in skip_keys:
            continue
        lines.append(f"- {key}: `{_format_solver_value(solver_report[key])}`")
    lines.append("")
    return lines


def _render_xychart(
    title: str,
    x_values: list[int],
    y_values: list[float],
    *,
    y_axis_label: str = "Value",
    clamp_y_min_zero: bool = False,
    integer_values: bool = False,
) -> list[str]:
    if not x_values or not y_values:
        return []
    y_min = min(y_values)
    y_max = max(y_values)
    if clamp_y_min_zero:
        y_min = min(0.0, y_min)
    if y_max == y_min:
        delta = 1.0 if y_max == 0 else abs(y_max) * 0.05
        if delta == 0:
            delta = 1.0
        y_min -= delta
        y_max += delta
    else:
        padding = (y_max - y_min) * 0.05
        y_min -= padding
        y_max += padding
    if clamp_y_min_zero:
        y_min = max(0.0, y_min)

    x_axis = ", ".join(str(value) for value in x_values)
    if integer_values:
        values = ", ".join(str(int(value)) for value in y_values)
        y_min_text = str(int(y_min))
        y_max_text = str(max(int(y_max), int(max(y_values, default=0))))
    else:
        values = ", ".join(f"{value:.6f}" for value in y_values)
        y_min_text = f"{y_min:.6f}"
        y_max_text = f"{y_max:.6f}"
    return [
        f"### {title}",
        "",
        "```mermaid",
        "xychart-beta",
        f'    title "{title}"',
        f'    x-axis "Generation" [{x_axis}]',
        f'    y-axis "{y_axis_label}" {y_min_text} --> {y_max_text}',
        f"    line [{values}]",
        "```",
        "",
    ]


def _nsga2_progress_section(metadata: dict | None) -> list[str]:
    if not metadata:
        return []
    solver_report = metadata.get("solver_report")
    if not isinstance(solver_report, dict):
        return []
    history = solver_report.get("generation_history")
    if not isinstance(history, list) or not history:
        return []
    backend = solver_report.get("solver_backend") or metadata.get("solver_backend")
    if backend != "Nsga2Solver":
        return []

    lines = [
        "## NSGA-II Progress",
        "",
        (
            "Read these curves as process health indicators: frontier "
            "growth shows whether search is still discovering new "
            "tradeoff points, parent-pool size shows how much valid "
            "material NSGA-II can breed from, and new-sample count "
            "shows how many fresh assignments each generation "
            "actually contributed."
        ),
        "",
    ]
    x_values = [int(item.get("generation", 0) or 0) for item in history]
    lines.extend(
        _render_xychart(
            "Frontier Size By Generation",
            x_values,
            [float(int(item.get("frontier_size", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    lines.extend(
        _render_xychart(
            "Parent Pool By Generation",
            x_values,
            [float(int(item.get("selected_parent_pool", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    lines.extend(
        _render_xychart(
            "New Samples By Generation",
            x_values,
            [float(int(item.get("generated_trials", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    return lines


def _ga_progress_section(metadata: dict | None) -> list[str]:
    if not metadata:
        return []
    solver_report = metadata.get("solver_report")
    if not isinstance(solver_report, dict):
        return []
    history = solver_report.get("generation_history")
    if not isinstance(history, list) or not history:
        return []
    backend = solver_report.get("solver_backend") or metadata.get("solver_backend")
    if backend != "GaSolver":
        return []

    lines = [
        "## GA Progress",
        "",
        (
            "Read these curves as process health indicators: best "
            "objective shows exploitation quality, mean objective "
            "shows whether the breeding pool is improving as a whole, "
            "elite seed count shows how much incumbent quality is "
            "being preserved, and new-sample count shows how much "
            "fresh exploration each generation still contributes."
        ),
        "",
    ]
    float_history = [
        item for item in history
        if item.get("best_objective") is not None and item.get("mean_objective") is not None
    ]
    if float_history:
        float_x = [int(item.get("generation", 0) or 0) for item in float_history]
        lines.extend(
            _render_xychart(
                "Best Objective By Generation",
                float_x,
                [float(item.get("best_objective", 0.0) or 0.0) for item in float_history],
                y_axis_label="Objective",
            )
        )
        lines.extend(
            _render_xychart(
                "Mean Objective By Generation",
                float_x,
                [float(item.get("mean_objective", 0.0) or 0.0) for item in float_history],
                y_axis_label="Objective",
            )
        )
    x_values = [int(item.get("generation", 0) or 0) for item in history]
    lines.extend(
        _render_xychart(
            "Elite Seeds By Generation",
            x_values,
            [float(int(item.get("elite_count", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    lines.extend(
        _render_xychart(
            "New Samples By Generation",
            x_values,
            [float(int(item.get("generated_trials", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    return lines


def _bayes_progress_section(metadata: dict | None) -> list[str]:
    if not metadata:
        return []
    solver_report = metadata.get("solver_report")
    if not isinstance(solver_report, dict):
        return []
    history = solver_report.get("generation_history")
    if not isinstance(history, list) or not history:
        return []
    backend = solver_report.get("solver_backend") or metadata.get("solver_backend")
    if backend != "BayesSolver":
        return []

    lines = [
        "## Bayesian Optimization Progress",
        "",
        (
            "Read these curves as process health indicators: best "
            "objective shows incumbent quality, observed-trial count "
            "shows how much real data has reached the surrogate, model-fit "
            "size shows the effective BO training set, and new-sample count "
            "shows how much fresh exploration each generation still emits."
        ),
        "",
    ]
    float_history = [
        item for item in history
        if item.get("best_objective") is not None
    ]
    if float_history:
        float_x = [int(item.get("generation", 0) or 0) for item in float_history]
        lines.extend(
            _render_xychart(
                "Best Objective By Generation",
                float_x,
                [float(item.get("best_objective", 0.0) or 0.0) for item in float_history],
                y_axis_label="Objective",
            )
        )
    x_values = [int(item.get("generation", 0) or 0) for item in history]
    lines.extend(
        _render_xychart(
            "Observed Trials By Generation",
            x_values,
            [float(int(item.get("observed_trials", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    lines.extend(
        _render_xychart(
            "Model Fit Size By Generation",
            x_values,
            [float(int(item.get("model_fit_size", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    lines.extend(
        _render_xychart(
            "New Samples By Generation",
            x_values,
            [float(int(item.get("generated_trials", 0) or 0)) for item in history],
            clamp_y_min_zero=True,
            integer_values=True,
        )
    )
    return lines


def _format_objective_map(
    trial: EvaluatedTrial,
    objectives: list[ObjectiveSpec],
) -> str:
    parts = []
    for objective in objectives:
        value = objective_value_for_trial(trial, objective)
        parts.append(f"{objective.key()}={_format_objective(value)}")
    return ", ".join(parts)


def _vtage_report_sections(history: list[EvaluatedTrial]) -> list[str]:
    valid = [
        trial for trial in history
        if trial.status == "valid" and trial.objective_value is not None
    ]
    if not valid:
        return ["## VTAGE Notes\n\nNo valid VTAGE trials yet."]

    best = max(valid, key=lambda trial: trial.objective_value)
    assignments = ", ".join(
        f"{name}={value}" for name, value in best.assignments.items()
    )
    return [
        "\n".join(
            [
                "## VTAGE Notes",
                "",
                f"- Best VTAGE trial: `{best.trial_id}`",
                f"- Best objective: `{best.objective_value:.6f}`",
                f"- Parameters: `{assignments}`",
            ]
        )
    ]


def _format_convergence_x_axis(series_length: int) -> str:
    if series_length <= 0:
        return 'x-axis "Valid Trial"'
    if series_length > MERMAID_CONVERGENCE_FULL_X_AXIS_LIMIT:
        return f'x-axis "Valid Trial" 1 --> {series_length}'
    x_axis = ", ".join(str(index) for index in range(1, series_length + 1))
    return f'x-axis "Valid Trial" [{x_axis}]'


def _mermaid_convergence_chart(problem: ParsedProblem, history: list[EvaluatedTrial]) -> list[str]:
    series = best_objective_series(problem, history)
    lines = ["## Charts", ""]
    if not series:
        lines.append("No valid objective values yet.")
        return lines

    values = ", ".join(f"{value:.6f}" for value in series)
    y_min = min(series)
    y_max = max(series)
    if y_max == y_min:
        delta = 1.0 if y_max == 0 else abs(y_max) * 0.05
        y_min -= delta
        y_max += delta
    else:
        padding = (y_max - y_min) * 0.05
        y_min -= padding
        y_max += padding
    lines.extend(
        [
            "### Convergence",
            "",
            "```mermaid",
            "xychart-beta",
            '    title "Best Objective So Far"',
            f"    {_format_convergence_x_axis(len(series))}",
            f'    y-axis "Objective" {y_min:.6f} --> {y_max:.6f}',
            f"    line [{values}]",
            "```",
            "",
        ]
    )

    counts = _status_counts(history)
    if counts:
        lines.extend(["### Trial Status", "", "```mermaid", "pie showData"])
        lines.append('    title "Trial Status Counts"')
        for status, count in sorted(counts.items()):
            lines.append(f'    "{status}" : {count}')
        lines.extend(["```", ""])
    return lines


def render_summary(
    problem: ParsedProblem,
    history: list[EvaluatedTrial],
    metadata: dict | None = None,
    extra_sections: list[str] | None = None,
) -> str:
    valid_count = sum(1 for trial in history if trial.status == "valid")
    invalid_count = sum(1 for trial in history if trial.status != "valid")
    objectives = problem.objective_list()
    primary = problem.primary_objective()
    best = best_trial(
        history,
        direction=primary.direction if primary is not None else "max",
        objective=primary,
        objectives=objectives,
    )
    frontier = pareto_frontier(history, objectives) if objectives else []
    lines = [
        "# Solver Summary",
        "",
        f"- Problem: `{problem.name}`",
        f"- Benchmark: `{problem.benchmark_type}`",
        f"- Objectives: `{'; '.join(objective.display_name() for objective in objectives)}`",
        f"- Trials: {len(history)} total, {valid_count} valid, {invalid_count} invalid",
    ]
    if problem.custom_bin:
        lines.append(f"- Custom bin: `{problem.custom_bin}`")
    elif problem.specific_benchmarks:
        lines.append(f"- Workload filter: `{problem.specific_benchmarks}`")
    if best is not None:
        lines.append(f"- Representative best: `{best.trial_id}`")
        lines.append(f"- Representative values: `{_format_objective_map(best, objectives)}`")
    if problem.is_multi_objective():
        lines.append(f"- Pareto frontier size: `{len(frontier)}`")
    lines.extend(["", "## Run Configuration", ""])
    lines.append(f"- Config path: `{problem.config_path}`")
    if metadata is not None:
        lines.append(f"- Problem ref: `{metadata.get('resolved_problem_ref', problem.problem_ref)}`")
        lines.append(f"- Solver kind: `{metadata.get('solver_kind', 'auto')}`")
        lines.append(f"- Solver backend: `{metadata.get('solver_backend', 'n/a')}`")
        lines.append(
            f"- max_parallel_trials: `{metadata.get('max_parallel_trials', 'n/a')}`"
        )
        lines.append(
            f"- max_parallel_workloads: `{metadata.get('max_parallel_workloads', 'n/a')}`"
        )
        lines.append(f"- gem5_build_type: `{metadata.get('gem5_build_type', 'n/a')}`")
        lines.append(f"- extra_args: `{metadata.get('extra_args', problem.extra_args)}`")
        lines.append(f"- stop_reason: `{metadata.get('stop_reason', 'n/a')}`")
    lines.extend(["", *_solver_algorithm_section(metadata)])
    lines.extend(["", *_nsga2_progress_section(metadata)])
    lines.extend(["", *_ga_progress_section(metadata)])
    lines.extend(["", *_bayes_progress_section(metadata)])
    lines.extend(["", * _mermaid_convergence_chart(problem, history), ""])
    if problem.is_multi_objective():
        diversity = crowding_distance(frontier, objectives)
        lines.extend(["## Pareto Frontier", ""])
        if not frontier:
            lines.append("No valid Pareto frontier points yet.")
        else:
            lines.append(
                "This table lists the current non-dominated trials. "
                "Use it to inspect the tradeoff surface, not to hunt "
                "for a single winner."
            )
            lines.append("")
            lines.append(
                "Higher `crowding_distance` means the point is more "
                "isolated on the current frontier and therefore "
                "contributes more diversity; `inf` marks boundary "
                "points at the edge of the frontier."
            )
            lines.append("")
            lines.append("| trial | crowding_distance | objectives | assignments |")
            lines.append("| --- | ---: | --- | --- |")
            for trial in frontier[:problem.summary_top_n]:
                assignments = ", ".join(
                    f"{key}={value}" for key, value in trial.assignments.items()
                )
                diversity_value = _format_solver_value(
                    diversity.get(trial.trial_id, 0.0)
                )
                objective_map = _format_objective_map(trial, objectives)
                lines.append(
                    f"| {trial.trial_id} | {diversity_value} | "
                    f"{objective_map} | {assignments} |"
                )
        lines.append("")
    for section in extra_sections or []:
        if not section.strip():
            continue
        lines.extend([section.rstrip(), ""])
    lines.extend(["## Top Results", ""])
    lines.append("| trial | objectives | status | assignments |")
    lines.append("| --- | ---: | --- | --- |")
    if problem.is_multi_objective():
        frontier_ids = {trial.trial_id for trial in frontier}
        ranked = sorted(
            history,
            key=lambda trial: (
                trial.status != "valid",
                trial.trial_id not in frontier_ids,
                trial.generation,
                trial.trial_id,
            ),
        )
    else:
        ranked = sorted(
            history,
            key=lambda trial: (
                trial.status != "valid",
                -(trial.objective_value or 0.0)
                if primary is not None and primary.direction == "max"
                else (trial.objective_value or 0.0),
            ),
        )
    for trial in ranked[:problem.summary_top_n]:
        assignments = ", ".join(f"{key}={value}" for key, value in trial.assignments.items())
        lines.append(
            f"| {trial.trial_id} | {_format_objective_map(trial, objectives)} | {trial.status} | {assignments} |"
        )
    return "\n".join(lines) + "\n"


def write_summary(path: str | Path, content: str) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def publish_step_summary(content: str) -> None:
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        Path(step_summary).write_text(content, encoding="utf-8")
