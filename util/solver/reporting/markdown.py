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
        "mutation_prob",
        "crossover_prob",
        "last_generation_mode",
        "last_population_size",
        "last_frontier_size",
        "last_selected_parent_pool",
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
        "population_size": "Target NSGA-II population size used to maintain the parent pool.",
        "mutation_prob": "Approximate fraction of parameters mutated when producing a child.",
        "crossover_prob": "Probability of recombining two parents instead of cloning them.",
        "last_generation_mode": "Whether the latest batch came from initial random sampling or offspring evolution.",
        "last_population_size": (
            "Number of valid historical trials available to the "
            "NSGA-II population builder before the latest propose step."
        ),
        "last_frontier_size": (
            "Current Pareto frontier size seen by the solver before "
            "generating the latest batch."
        ),
        "last_selected_parent_pool": (
            "How many individuals were retained as the parent pool "
            "for the latest offspring step."
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


def _nsga2_progress_section(metadata: dict | None) -> list[str]:
    if not metadata:
        return []
    solver_report = metadata.get("solver_report")
    if not isinstance(solver_report, dict):
        return []
    history = solver_report.get("generation_history")
    if not isinstance(history, list) or not history:
        return []

    def chart(title: str, key: str) -> list[str]:
        x_axis = ", ".join(str(item.get("generation", 0)) for item in history)
        value_list = [int(item.get(key, 0) or 0) for item in history]
        values = ", ".join(str(value) for value in value_list)
        y_max = max(value_list, default=1)
        if y_max <= 0:
            y_max = 1
        return [
            f"### {title}",
            "",
            "```mermaid",
            "xychart-beta",
            f'    title "{title}"',
            f'    x-axis "Generation" [{x_axis}]',
            f'    y-axis "Value" 0 --> {y_max}',
            f"    line [{values}]",
            "```",
            "",
        ]

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
    lines.extend(chart("Frontier Size By Generation", "frontier_size"))
    lines.extend(chart("Parent Pool By Generation", "selected_parent_pool"))
    lines.extend(chart("New Samples By Generation", "generated_trials"))
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


def _mermaid_convergence_chart(problem: ParsedProblem, history: list[EvaluatedTrial]) -> list[str]:
    series = best_objective_series(problem, history)
    lines = ["## Charts", ""]
    if not series:
        lines.append("No valid objective values yet.")
        return lines

    x_axis = ", ".join(str(index) for index in range(1, len(series) + 1))
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
            f'    x-axis "Valid Trial" [{x_axis}]',
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
