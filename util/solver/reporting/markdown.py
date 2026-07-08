from __future__ import annotations

import os
from pathlib import Path

from util.solver.processing.aggregate import best_trial
from util.solver.types import EvaluatedTrial, ParsedProblem


def render_summary(problem: ParsedProblem, history: list[EvaluatedTrial]) -> str:
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
        lines.append(f"- Best: `{best.trial_id}` = `{best.objective_value}`")
    lines.extend(["", "## Top Results", ""])
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
    for trial in ranked[:10]:
        assignments = ", ".join(f"{key}={value}" for key, value in trial.assignments.items())
        lines.append(
            f"| {trial.trial_id} | {trial.objective_value} | {trial.status} | {assignments} |"
        )
    return "\n".join(lines) + "\n"


def write_summary(path: str | Path, content: str) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as handle:
            handle.write(content)
            handle.write("\n")
