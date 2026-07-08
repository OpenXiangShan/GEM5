from __future__ import annotations

import os
from pathlib import Path

from util.solver.processing.aggregate import best_trial
from util.solver.reporting.charts import best_objective_series
from util.solver.types import EvaluatedTrial, ParsedProblem


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


def _status_counts(history: list[EvaluatedTrial]) -> dict[str, int]:
    counts = {}
    for trial in history:
        counts[trial.status] = counts.get(trial.status, 0) + 1
    return counts


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
    extra_sections: list[str] | None = None,
) -> str:
    valid_count = sum(1 for trial in history if trial.status == "valid")
    invalid_count = sum(1 for trial in history if trial.status != "valid")
    best = best_trial(history, direction=problem.objective.direction)
    lines = [
        "# Solver Summary",
        "",
        f"- Problem: `{problem.name}`",
        f"- Benchmark: `{problem.benchmark_type}`",
        f"- Objective: `{problem.objective.source_kind}:{problem.objective.metric}` ({problem.objective.direction})",
        f"- Trials: {len(history)} total, {valid_count} valid, {invalid_count} invalid",
    ]
    if problem.custom_bin:
        lines.append(f"- Custom bin: `{problem.custom_bin}`")
    elif problem.specific_benchmarks:
        lines.append(f"- Workload filter: `{problem.specific_benchmarks}`")
    if best is not None:
        lines.append(f"- Best: `{best.trial_id}` = `{_format_objective(best.objective_value)}`")
    lines.extend(["", * _mermaid_convergence_chart(problem, history), ""])
    for section in extra_sections or []:
        if not section.strip():
            continue
        lines.extend([section.rstrip(), ""])
    lines.extend(["## Top Results", ""])
    lines.append("| trial | objective | status | assignments |")
    lines.append("| --- | ---: | --- | --- |")
    ranked = sorted(
        history,
        key=lambda trial: (
            trial.status != "valid",
            -(trial.objective_value or 0.0)
            if problem.objective.direction == "max"
            else (trial.objective_value or 0.0),
        ),
    )
    for trial in ranked[:problem.summary_top_n]:
        assignments = ", ".join(f"{key}={value}" for key, value in trial.assignments.items())
        lines.append(
            f"| {trial.trial_id} | {_format_objective(trial.objective_value)} | {trial.status} | {assignments} |"
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
