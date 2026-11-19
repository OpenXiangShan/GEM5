#!/usr/bin/env python3

import argparse
import csv
import json
import pathlib
import sys
from typing import Dict, Iterable, List, Mapping, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.offset_distribution import rect, svg_document, text


BASELINE = "merged_d2_r50_s31"
REFERENCE_LABELS = {
    "dual": "Dual BOP",
    "merged_baseline": "Merged D2 R50/S31",
}
PALETTE = ("#d1495b", "#f28e2b", "#00a676", "#8f5bb7", "#4e79a7")


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def scheme_label(document: Mapping, name: str) -> str:
    config = document["aggregate"]["schemes"][name]["config"]
    return f"R{config['round_max']}/S{config['score_max']}"


def checkpoint_design(checkpoint: Mapping, name: str, kind: str) -> Mapping:
    container = checkpoint["references"] if kind == "reference" else checkpoint["schemes"]
    return container[name]


def aggregate_design(document: Mapping, name: str, kind: str) -> Mapping:
    container = (
        document["aggregate"]["references"]
        if kind == "reference"
        else document["aggregate"]["schemes"]
    )
    return container[name]


def ratio(metrics: Mapping, numerator: str) -> float:
    denominator = metrics["demand_count"]
    return metrics[numerator] / denominator if denominator else 0.0


def sent_per_demand(metrics: Mapping) -> float:
    denominator = metrics["demand_count"]
    sent = sum(metrics.get("sent_by_role", {}).values())
    return sent / denominator if denominator else 0.0


def all_designs(document: Mapping) -> List[Tuple[str, str, str]]:
    designs = [
        (name, "reference", label)
        for name, label in REFERENCE_LABELS.items()
    ]
    designs.extend(
        (name, "scheme", scheme_label(document, name))
        for name in sorted(
            document["aggregate"]["schemes"],
            key=lambda item: (
                document["aggregate"]["schemes"][item]["config"]["round_max"],
                document["aggregate"]["schemes"][item]["config"]["score_max"],
            ),
        )
    )
    return designs


def write_checkpoint_csv(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    for checkpoint in document["checkpoints"]:
        for name, kind, label in all_designs(document):
            design = checkpoint_design(checkpoint, name, kind)
            config = design.get("config", {})
            phase = design.get("phase", {})
            rows.append({
                "checkpoint": checkpoint["name"],
                "weight": checkpoint["weight"],
                "design": label,
                "kind": kind,
                "round_max": config.get("round_max", ""),
                "score_max": config.get("score_max", ""),
                "ideal_coverage": design["metrics"]["coverage"],
                "local_coverage": design["local_metrics"]["coverage"],
                "generated_per_demand": ratio(
                    design["metrics"], "generated_count"
                ),
                "sent_per_demand": sent_per_demand(design["local_metrics"]),
                "phase_count": phase.get("phase_count", ""),
                "average_rounds": phase.get("average_rounds", ""),
                "average_best_score": phase.get("average_best_score", ""),
                "score_max_end_fraction": phase.get(
                    "score_max_end_fraction", ""
                ),
                "issue_enabled_fraction": phase.get(
                    "issue_enabled_fraction", ""
                ),
            })
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_aggregate_csv(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    for name, kind, label in all_designs(document):
        design = aggregate_design(document, name, kind)
        config = design.get("config", {})
        phase = design.get("phase", {})
        for weighting in ("checkpoint_weighted", "access_weighted"):
            rows.append({
                "design": label,
                "kind": kind,
                "round_max": config.get("round_max", ""),
                "score_max": config.get("score_max", ""),
                "weighting": weighting,
                "ideal_coverage": design["ideal_coverage"][weighting],
                "local_coverage": design["local_coverage"][weighting],
                "generated_per_demand": design["generated_per_demand"][weighting],
                "sent_per_demand": design["local_sent_per_demand"][weighting],
                "phase_per_million_demands": phase.get(
                    "per_million_demands", ""
                ),
                "average_rounds": phase.get("average_rounds", ""),
                "average_best_score": phase.get("average_best_score", ""),
                "score_max_end_fraction": phase.get(
                    "score_max_end_fraction", ""
                ),
                "issue_enabled_fraction": phase.get(
                    "issue_enabled_fraction", ""
                ),
            })
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def blend(lhs: str, rhs: str, amount: float) -> str:
    amount = max(0.0, min(1.0, amount))
    lhs_rgb = tuple(int(lhs[index:index + 2], 16) for index in (1, 3, 5))
    rhs_rgb = tuple(int(rhs[index:index + 2], 16) for index in (1, 3, 5))
    rgb = tuple(
        round(left + (right - left) * amount)
        for left, right in zip(lhs_rgb, rhs_rgb)
    )
    return "#" + "".join(f"{value:02x}" for value in rgb)


def diverging_color(value: float, limit: float) -> str:
    if limit <= 0:
        return "#ffffff"
    strength = min(1.0, abs(value) / limit)
    target = "#248f5a" if value >= 0 else "#c94b50"
    return blend("#ffffff", target, 0.15 + 0.75 * strength)


def sequential_color(value: float, minimum: float, maximum: float) -> str:
    fraction = (value - minimum) / (maximum - minimum) if maximum > minimum else 0.5
    return blend("#edf4fb", "#2f6ca8", 0.12 + 0.78 * fraction)


def param_grid(document: Mapping) -> Tuple[List[int], List[int], Dict[Tuple[int, int], str]]:
    rounds = set()
    scores = set()
    lookup = {}
    for name, design in document["aggregate"]["schemes"].items():
        config = design["config"]
        key = (config["round_max"], config["score_max"])
        rounds.add(key[0])
        scores.add(key[1])
        lookup[key] = name
    return sorted(rounds), sorted(scores), lookup


def render_coverage_heatmap(document: Mapping, path: pathlib.Path) -> None:
    rounds, scores, lookup = param_grid(document)
    baseline = document["aggregate"]["schemes"][BASELINE]
    modes = (
        ("checkpoint_weighted", "Checkpoint-weighted local coverage"),
        ("access_weighted", "Access-weighted local coverage"),
    )
    deltas = []
    for mode, _ in modes:
        base_value = baseline["local_coverage"][mode]
        deltas.extend(
            design["local_coverage"][mode] - base_value
            for design in document["aggregate"]["schemes"].values()
        )
    limit = max(0.005, max(abs(value) for value in deltas))
    width = 1380
    height = 160 + len(rounds) * 58
    panel_width = 640
    origins = (40, 720)
    cell_width = min(118, (panel_width - 120) / len(scores))
    cell_height = 48
    body = [
        text(24, 30, "Merged VBOP Teacher parameter sweep", "title"),
        text(
            24,
            51,
            "Cell labels are local coverage deltas versus R50/S31 after the shared filter, FIFOs, and 3:1 arbiter.",
            "subtitle",
        ),
    ]
    for (mode, title), origin_x in zip(modes, origins):
        chart_x = origin_x + 95
        chart_y = 105
        body.append(text(origin_x, 82, title, "label"))
        for column, score in enumerate(scores):
            x = chart_x + column * cell_width + cell_width / 2
            body.append(text(x, chart_y - 12, f"S{score}", "small", "middle"))
        base_value = baseline["local_coverage"][mode]
        for row, round_max in enumerate(rounds):
            y = chart_y + row * cell_height
            body.append(
                text(chart_x - 12, y + 29, f"R{round_max}", "small", "end")
            )
            for column, score_max in enumerate(scores):
                x = chart_x + column * cell_width
                name = lookup.get((round_max, score_max))
                if name is None:
                    body.append(rect(x + 2, y + 2, cell_width - 4, cell_height - 4, "#f2f4f7"))
                    body.append(text(x + cell_width / 2, y + 29, "n/a", "small", "middle"))
                    continue
                value = document["aggregate"]["schemes"][name]["local_coverage"][mode]
                delta = value - base_value
                body.append(
                    rect(
                        x + 2,
                        y + 2,
                        cell_width - 4,
                        cell_height - 4,
                        diverging_color(delta, limit),
                    )
                )
                if name == BASELINE:
                    body.append(
                        f'<rect x="{x + 2:.1f}" y="{y + 2:.1f}" '
                        f'width="{cell_width - 4:.1f}" height="{cell_height - 4:.1f}" '
                        'fill="none" stroke="#111827" stroke-width="2"/>'
                    )
                body.append(
                    text(
                        x + cell_width / 2,
                        y + 29,
                        f"{delta * 100:+.2f} pp",
                        "label",
                        "middle",
                    )
                )
        body.append(
            text(
                chart_x + len(scores) * cell_width / 2,
                chart_y + len(rounds) * cell_height + 24,
                f"baseline {base_value * 100:.2f}%",
                "small",
                "middle",
            )
        )
    path.write_text(svg_document(width, height, body))


def pareto_names(document: Mapping, mode: str) -> List[str]:
    schemes = document["aggregate"]["schemes"]
    frontier = []
    for name, design in schemes.items():
        coverage = design["local_coverage"][mode]
        sent = design["local_sent_per_demand"][mode]
        dominated = any(
            other["local_coverage"][mode] >= coverage
            and other["local_sent_per_demand"][mode] <= sent
            and (
                other["local_coverage"][mode] > coverage
                or other["local_sent_per_demand"][mode] < sent
            )
            for other_name, other in schemes.items()
            if other_name != name
        )
        if not dominated:
            frontier.append(name)
    return frontier


def select_key_schemes(document: Mapping) -> List[str]:
    schemes = document["aggregate"]["schemes"]
    dual = document["aggregate"]["references"]["dual"]
    candidates = [BASELINE]
    candidates.append(max(
        schemes,
        key=lambda name: schemes[name]["local_coverage"]["checkpoint_weighted"],
    ))
    candidates.append(max(
        schemes,
        key=lambda name: schemes[name]["local_coverage"]["access_weighted"],
    ))
    capped = [
        name
        for name, design in schemes.items()
        if design["local_sent_per_demand"]["checkpoint_weighted"]
        <= dual["local_sent_per_demand"]["checkpoint_weighted"]
        and design["local_sent_per_demand"]["access_weighted"]
        <= dual["local_sent_per_demand"]["access_weighted"]
    ]
    if capped:
        candidates.append(max(
            capped,
            key=lambda name: (
                schemes[name]["local_coverage"]["checkpoint_weighted"]
                + schemes[name]["local_coverage"]["access_weighted"]
            ),
        ))
    frontier = set(pareto_names(document, "checkpoint_weighted")) | set(
        pareto_names(document, "access_weighted")
    )
    for name in sorted(
        frontier,
        key=lambda item: (
            schemes[item]["local_coverage"]["checkpoint_weighted"]
            + schemes[item]["local_coverage"]["access_weighted"]
        ),
        reverse=True,
    ):
        candidates.append(name)
    result = []
    for name in candidates:
        if name not in result:
            result.append(name)
        if len(result) == 5:
            break
    return result


def padded_bounds(values: Iterable[float], fraction: float = 0.12) -> Tuple[float, float]:
    values = list(values)
    minimum = min(values)
    maximum = max(values)
    span = maximum - minimum
    padding = span * fraction if span else max(abs(maximum) * 0.05, 0.01)
    return minimum - padding, maximum + padding


def render_tradeoff(document: Mapping, path: pathlib.Path) -> None:
    selected = select_key_schemes(document)
    selected_colors = {
        name: PALETTE[index % len(PALETTE)]
        for index, name in enumerate(selected)
    }
    width = 1420
    height = 630
    panel_width = 630
    panel_height = 440
    origins = ((65, 120), (760, 120))
    modes = (
        ("checkpoint_weighted", "Checkpoint-weighted"),
        ("access_weighted", "Access-weighted"),
    )
    body = [
        text(24, 30, "Teacher parameters: local coverage / traffic trade-off", "title"),
        text(
            24,
            51,
            "All gray points are scanned parameter pairs; colored points mark the baseline and key frontier points.",
            "subtitle",
        ),
    ]
    legend_x = 24
    for name in selected:
        color = selected_colors[name]
        body.append(rect(legend_x, 68, 13, 13, color))
        body.append(text(legend_x + 19, 79, scheme_label(document, name), "small"))
        legend_x += 125
    body.append(rect(legend_x, 68, 13, 13, "#3448c5"))
    body.append(text(legend_x + 19, 79, "Dual BOP", "small"))

    for (mode, title), (origin_x, origin_y) in zip(modes, origins):
        scheme_points = [
            (
                design["local_sent_per_demand"][mode],
                design["local_coverage"][mode],
                name,
            )
            for name, design in document["aggregate"]["schemes"].items()
        ]
        dual = document["aggregate"]["references"]["dual"]
        all_points = scheme_points + [(
            dual["local_sent_per_demand"][mode],
            dual["local_coverage"][mode],
            "dual",
        )]
        x_min, x_max = padded_bounds(point[0] for point in all_points)
        y_min, y_max = padded_bounds(point[1] for point in all_points)
        plot_x = origin_x + 58
        plot_y = origin_y + 32
        plot_width = panel_width - 90
        plot_height = panel_height - 92
        body.append(text(origin_x, origin_y, title, "label"))
        for tick_index in range(6):
            fraction = tick_index / 5
            x = plot_x + fraction * plot_width
            y = plot_y + plot_height - fraction * plot_height
            x_value = x_min + fraction * (x_max - x_min)
            y_value = y_min + fraction * (y_max - y_min)
            body.append(
                f'<line x1="{x:.1f}" y1="{plot_y:.1f}" x2="{x:.1f}" '
                f'y2="{plot_y + plot_height:.1f}" class="axis"/>'
            )
            body.append(
                f'<line x1="{plot_x:.1f}" y1="{y:.1f}" '
                f'x2="{plot_x + plot_width:.1f}" y2="{y:.1f}" class="axis"/>'
            )
            body.append(text(x, plot_y + plot_height + 20, f"{x_value:.2f}", "small", "middle"))
            body.append(text(plot_x - 8, y + 4, f"{y_value * 100:.1f}%", "small", "end"))
        body.append(text(plot_x, plot_y - 10, "local coverage", "small"))
        body.append(text(
            plot_x + plot_width / 2,
            plot_y + plot_height + 43,
            "local sent / demand",
            "small",
            "middle",
        ))
        for sent, coverage, name in scheme_points:
            x = plot_x + (sent - x_min) / (x_max - x_min) * plot_width
            y = plot_y + plot_height - (coverage - y_min) / (y_max - y_min) * plot_height
            color = selected_colors.get(name, "#aab2bf")
            radius = 6 if name in selected_colors else 3.5
            opacity = "1" if name in selected_colors else "0.65"
            body.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{radius}" '
                f'fill="{color}" fill-opacity="{opacity}" stroke="#ffffff" stroke-width="1"/>'
            )
        dual_sent = dual["local_sent_per_demand"][mode]
        dual_coverage = dual["local_coverage"][mode]
        x = plot_x + (dual_sent - x_min) / (x_max - x_min) * plot_width
        y = plot_y + plot_height - (dual_coverage - y_min) / (y_max - y_min) * plot_height
        body.append(
            f'<circle cx="{x:.1f}" cy="{y:.1f}" r="7" fill="#3448c5" '
            'stroke="#ffffff" stroke-width="1.5"/>'
        )
        body.append(text(x + 8, y - 9, "Dual BOP", "small"))
    path.write_text(svg_document(width, height, body))


def render_phase_heatmap(document: Mapping, path: pathlib.Path) -> None:
    rounds, scores, lookup = param_grid(document)
    panels = (
        ("average_rounds", "Actual average rounds per Teacher phase", lambda value: f"{value:.1f}"),
        ("score_max_end_fraction", "Phases ended by score_max", lambda value: f"{value * 100:.0f}%"),
    )
    values_by_metric = {
        metric: [
            design["phase"][metric]
            for design in document["aggregate"]["schemes"].values()
        ]
        for metric, _, _ in panels
    }
    width = 1380
    height = 160 + len(rounds) * 58
    panel_width = 640
    origins = (40, 720)
    cell_width = min(118, (panel_width - 120) / len(scores))
    cell_height = 48
    body = [
        text(24, 30, "Teacher phase behavior", "title"),
        text(
            24,
            51,
            "score_max can end a strong phase early; round_max caps phases that never reach that score.",
            "subtitle",
        ),
    ]
    for (metric, title, formatter), origin_x in zip(panels, origins):
        chart_x = origin_x + 95
        chart_y = 105
        values = values_by_metric[metric]
        minimum, maximum = min(values), max(values)
        body.append(text(origin_x, 82, title, "label"))
        for column, score in enumerate(scores):
            x = chart_x + column * cell_width + cell_width / 2
            body.append(text(x, chart_y - 12, f"S{score}", "small", "middle"))
        for row, round_max in enumerate(rounds):
            y = chart_y + row * cell_height
            body.append(text(chart_x - 12, y + 29, f"R{round_max}", "small", "end"))
            for column, score_max in enumerate(scores):
                x = chart_x + column * cell_width
                name = lookup.get((round_max, score_max))
                if name is None:
                    body.append(rect(x + 2, y + 2, cell_width - 4, cell_height - 4, "#f2f4f7"))
                    body.append(text(x + cell_width / 2, y + 29, "n/a", "small", "middle"))
                    continue
                value = document["aggregate"]["schemes"][name]["phase"][metric]
                body.append(rect(
                    x + 2,
                    y + 2,
                    cell_width - 4,
                    cell_height - 4,
                    sequential_color(value, minimum, maximum),
                ))
                if name == BASELINE:
                    body.append(
                        f'<rect x="{x + 2:.1f}" y="{y + 2:.1f}" '
                        f'width="{cell_width - 4:.1f}" height="{cell_height - 4:.1f}" '
                        'fill="none" stroke="#111827" stroke-width="2"/>'
                    )
                body.append(text(x + cell_width / 2, y + 29, formatter(value), "label", "middle"))
    path.write_text(svg_document(width, height, body))


def render_checkpoint_delta(document: Mapping, path: pathlib.Path) -> None:
    selected = select_key_schemes(document)
    compared = [name for name in selected if name != BASELINE]
    columns = [("dual", "reference", "Dual BOP")]
    columns.extend(
        (name, "scheme", scheme_label(document, name)) for name in compared
    )
    checkpoints = document["checkpoints"]
    deltas = []
    for checkpoint in checkpoints:
        baseline = checkpoint["schemes"][BASELINE]["local_metrics"]["coverage"]
        for name, kind, _ in columns:
            value = checkpoint_design(checkpoint, name, kind)["local_metrics"]["coverage"]
            deltas.append(value - baseline)
    limit = max(0.01, max(abs(value) for value in deltas))
    cell_width = 145
    cell_height = 43
    chart_x = 245
    chart_y = 110
    width = chart_x + cell_width * len(columns) + 35
    height = chart_y + cell_height * len(checkpoints) + 48
    body = [
        text(24, 30, "Per-checkpoint local coverage delta", "title"),
        text(
            24,
            51,
            "Deltas are versus merged D2 R50/S31; positive cells recover coverage, negative cells regress.",
            "subtitle",
        ),
    ]
    for column, (_, _, label) in enumerate(columns):
        x = chart_x + column * cell_width + cell_width / 2
        body.append(text(x, chart_y - 18, label, "small", "middle"))
    for row, checkpoint in enumerate(checkpoints):
        y = chart_y + row * cell_height
        baseline = checkpoint["schemes"][BASELINE]["local_metrics"]["coverage"]
        body.append(text(18, y + 27, checkpoint["name"], "label"))
        body.append(text(chart_x - 12, y + 27, f"base {baseline * 100:.1f}%", "small", "end"))
        for column, (name, kind, _) in enumerate(columns):
            x = chart_x + column * cell_width
            value = checkpoint_design(checkpoint, name, kind)["local_metrics"]["coverage"]
            delta = value - baseline
            body.append(rect(
                x + 2,
                y + 2,
                cell_width - 4,
                cell_height - 4,
                diverging_color(delta, limit),
            ))
            body.append(text(
                x + cell_width / 2,
                y + 27,
                f"{delta * 100:+.2f} pp",
                "label",
                "middle",
            ))
    path.write_text(svg_document(width, height, body))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate tables and SVGs for TS-BOP Teacher parameter sweep"
    )
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--out-dir", type=pathlib.Path, required=True)
    parser.add_argument(
        "--prefix",
        default="ts_bop_teacher_param",
        help="output filename prefix",
    )
    args = parser.parse_args()

    document = load_json(args.input)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_checkpoint_csv(
        document, args.out_dir / f"{args.prefix}_checkpoint.csv"
    )
    write_aggregate_csv(
        document, args.out_dir / f"{args.prefix}_aggregate.csv"
    )
    render_coverage_heatmap(
        document, args.out_dir / f"{args.prefix}_coverage_heatmap.svg"
    )
    render_tradeoff(
        document, args.out_dir / f"{args.prefix}_tradeoff.svg"
    )
    render_phase_heatmap(
        document, args.out_dir / f"{args.prefix}_phase_heatmap.svg"
    )
    render_checkpoint_delta(
        document, args.out_dir / f"{args.prefix}_checkpoint_delta.svg"
    )
    print(json.dumps({
        "selected_schemes": [
            scheme_label(document, name) for name in select_key_schemes(document)
        ],
        "pareto_checkpoint_weighted": [
            scheme_label(document, name)
            for name in pareto_names(document, "checkpoint_weighted")
        ],
        "pareto_access_weighted": [
            scheme_label(document, name)
            for name in pareto_names(document, "access_weighted")
        ],
    }, indent=2))


if __name__ == "__main__":
    main()
