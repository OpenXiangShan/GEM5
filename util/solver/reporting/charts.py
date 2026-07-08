from __future__ import annotations

from pathlib import Path

from util.solver.types import EvaluatedTrial, ParsedProblem


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
    return [str(path) for path in paths]


def _render_objective_chart(problem: ParsedProblem, history: list[EvaluatedTrial], path: Path) -> Path:
    width = 640
    height = 240
    margin = 40
    lines = _svg_header(width, height)
    values = []
    best_so_far = None
    for trial in history:
        if trial.status != "valid" or trial.objective_value is None:
            continue
        if best_so_far is None:
            best_so_far = trial.objective_value
        elif problem.objective.direction == "max":
            best_so_far = max(best_so_far, trial.objective_value)
        else:
            best_so_far = min(best_so_far, trial.objective_value)
        values.append(best_so_far)

    lines.append(
        f'<text x="{margin}" y="20" font-size="16">Best objective so far</text>'
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
