from __future__ import annotations

from pathlib import Path

from util.solver.processing.aggregate import objective_value_for_trial, pareto_frontier
from util.solver.types import EvaluatedTrial, ParsedProblem


def best_objective_series(
    problem: ParsedProblem,
    history: list[EvaluatedTrial],
) -> list[float]:
    primary = problem.primary_objective()
    if primary is None:
        return []
    values = []
    best_so_far = None
    for trial in history:
        value = objective_value_for_trial(trial, primary)
        if trial.status != "valid" or value is None:
            continue
        if best_so_far is None:
            best_so_far = value
        elif primary.direction == "max":
            best_so_far = max(best_so_far, value)
        else:
            best_so_far = min(best_so_far, value)
        values.append(best_so_far)
    return values


def frontier_size_series(
    problem: ParsedProblem,
    history: list[EvaluatedTrial],
) -> list[int]:
    objectives = problem.objective_list()
    if not objectives:
        return []
    values = []
    for index in range(len(history)):
        prefix = history[: index + 1]
        values.append(len(pareto_frontier(prefix, objectives)))
    return values


def _svg_header(width: int, height: int) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
    ]


def _write_svg(path: Path, lines: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines + ["</svg>", ""]), encoding="utf-8")


def render_charts(problem: ParsedProblem, history: list[EvaluatedTrial], outdir: str | Path) -> list[str]:
    outdir = Path(outdir)
    paths = [
        _render_objective_chart(problem, history, outdir / "best_objective.svg"),
        _render_status_chart(history, outdir / "trial_status.svg"),
    ]
    if problem.is_multi_objective():
        paths.append(
            _render_frontier_size_chart(problem, history, outdir / "pareto_frontier_size.svg")
        )
    return [str(path) for path in paths]


def _render_objective_chart(problem: ParsedProblem, history: list[EvaluatedTrial], path: Path) -> Path:
    width = 640
    height = 240
    margin = 40
    lines = _svg_header(width, height)
    values = best_objective_series(problem, history)

    title = "Best primary objective so far"
    if not problem.is_multi_objective():
        title = "Best objective so far"
    lines.append(
        f'<text x="{margin}" y="20" font-size="16">{title}</text>'
    )
    lines.append(
        f'<line x1="{margin}" y1="{height - margin}" x2="{width - margin}" y2="{height - margin}" stroke="#444"/>'
    )
    lines.append(
        f'<line x1="{margin}" y1="{margin}" x2="{margin}" y2="{height - margin}" stroke="#444"/>'
    )
    if not values:
        lines.append(
            f'<text x="{margin}" y="{height / 2}" font-size="14">'
            "No valid objective values yet"
            "</text>"
        )
        _write_svg(path, lines)
        return path

    min_value = min(values)
    max_value = max(values)
    if max_value == min_value:
        max_value += 1.0
    x_step = (width - 2 * margin) / max(1, len(values) - 1)
    points = []
    for index, value in enumerate(values):
        x = margin + index * x_step
        ratio = (value - min_value) / (max_value - min_value)
        y = (height - margin) - ratio * (height - 2 * margin)
        points.append(f"{x:.2f},{y:.2f}")
    lines.append(
        f'<polyline fill="none" stroke="#0b6efd" stroke-width="2" points="{" ".join(points)}"/>'
    )
    lines.append(
        f'<text x="{width - margin}" y="{margin}" text-anchor="end" font-size="12">{max(values):.4f}</text>'
    )
    lines.append(
        f'<text x="{width - margin}" y="{height - margin + 16}" '
        f'text-anchor="end" font-size="12">{len(values)} valid trials</text>'
    )
    _write_svg(path, lines)
    return path


def _render_frontier_size_chart(problem: ParsedProblem, history: list[EvaluatedTrial], path: Path) -> Path:
    width = 640
    height = 240
    margin = 40
    lines = _svg_header(width, height)
    values = frontier_size_series(problem, history)

    lines.append(
        f'<text x="{margin}" y="20" font-size="16">Pareto frontier size so far</text>'
    )
    lines.append(
        f'<line x1="{margin}" y1="{height - margin}" x2="{width - margin}" y2="{height - margin}" stroke="#444"/>'
    )
    lines.append(
        f'<line x1="{margin}" y1="{margin}" x2="{margin}" y2="{height - margin}" stroke="#444"/>'
    )
    if not values:
        lines.append(
            f'<text x="{margin}" y="{height / 2}" font-size="14">No trials yet</text>'
        )
        _write_svg(path, lines)
        return path

    min_value = min(values)
    max_value = max(values)
    if max_value == min_value:
        max_value += 1.0
    x_step = (width - 2 * margin) / max(1, len(values) - 1)
    points = []
    for index, value in enumerate(values):
        x = margin + index * x_step
        ratio = (value - min_value) / (max_value - min_value)
        y = (height - margin) - ratio * (height - 2 * margin)
        points.append(f"{x:.2f},{y:.2f}")
    lines.append(
        f'<polyline fill="none" stroke="#dc3545" stroke-width="2" points="{" ".join(points)}"/>'
    )
    lines.append(
        f'<text x="{width - margin}" y="{margin}" text-anchor="end" font-size="12">{values[-1]} frontier points</text>'
    )
    _write_svg(path, lines)
    return path


def _render_status_chart(history: list[EvaluatedTrial], path: Path) -> Path:
    width = 400
    height = 240
    margin = 40
    lines = _svg_header(width, height)
    counts = {}
    for trial in history:
        counts[trial.status] = counts.get(trial.status, 0) + 1

    labels = sorted(counts) or ["valid"]
    max_count = max(counts.values(), default=1)
    bar_width = 40
    gap = 30
    start_x = margin
    lines.append(f'<text x="{margin}" y="20" font-size="16">Trial status counts</text>')
    for index, label in enumerate(labels):
        count = counts.get(label, 0)
        x = start_x + index * (bar_width + gap)
        bar_height = 0 if max_count == 0 else (count / max_count) * (height - 2 * margin)
        y = height - margin - bar_height
        lines.append(
            f'<rect x="{x}" y="{y}" width="{bar_width}" height="{bar_height}" fill="#198754"/>'
        )
        lines.append(
            f'<text x="{x + bar_width / 2}" y="{height - margin + 16}" '
            f'text-anchor="middle" font-size="12">{label}</text>'
        )
        lines.append(
            f'<text x="{x + bar_width / 2}" y="{y - 6}" text-anchor="middle" font-size="12">{count}</text>'
        )
    _write_svg(path, lines)
    return path
