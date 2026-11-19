#!/usr/bin/env python3

import argparse
import csv
import json
import math
import pathlib
import sys
from typing import Dict, Iterable, List, Mapping, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.offset_distribution import (
    rect,
    svg_document,
    text,
)


BASE_DESIGNS = (
    ("dual", "reference", "Dual BOP", "#3448c5"),
    ("merged_baseline", "reference", "Merged D2", "#758195"),
)

POOL4_DESIGNS = BASE_DESIGNS + (
    ("partition_v2_p2_independent", "scheme", "Strict 2+2", "#d1495b"),
    ("partition_v2_p2_max_union", "scheme", "Strict 2+2, max-union", "#f28e2b"),
    ("reserve_v1_p1_t4_independent", "scheme", "Reserve 1+1 / T4", "#00a676"),
    ("reserve_v1_p1_t4_max_union", "scheme", "Reserve 1+1 / T4, max-union", "#8f5bb7"),
)

POOL6_DESIGNS = BASE_DESIGNS + (
    ("partition_v3_p3_independent", "scheme", "Strict 3+3", "#f28e2b"),
    ("partition_v3_p3_max_union", "scheme", "Strict 3+3, max-union", "#d1495b"),
    ("reserve_v2_p2_t6_independent", "scheme", "Reserve 2+2 / T6", "#00a676"),
    ("reserve_v2_p2_t6_max_union", "scheme", "Reserve 2+2 / T6, max-union", "#8f5bb7"),
)

TRADEOFF_DESIGNS = BASE_DESIGNS + (
    ("partition_v2_p2_max_union", "scheme", "P2+2 max-union", "#d1495b"),
    ("reserve_v1_p1_t4_max_union", "scheme", "R1+1/T4 max-union", "#00a676"),
    ("partition_v3_p3_max_union", "scheme", "P3+3 max-union", "#f28e2b"),
    ("reserve_v2_p2_t6_max_union", "scheme", "R2+2/T6 max-union", "#8f5bb7"),
)

TRADEOFF_LABEL_OFFSETS = {
    "checkpoint_weighted": {
        "Dual BOP": (8, -9),
        "Merged D2": (8, 17),
        "P2+2 max-union": (8, 19),
        "R1+1/T4 max-union": (8, 31),
        "P3+3 max-union": (8, -10),
        "R2+2/T6 max-union": (-112, 18),
    },
    "access_weighted": {
        "Dual BOP": (8, -9),
        "Merged D2": (8, 17),
        "P2+2 max-union": (8, -10),
        "R1+1/T4 max-union": (8, 20),
        "P3+3 max-union": (8, -10),
        "R2+2/T6 max-union": (8, 20),
    },
}

SCHEME_LABELS = {
    "partition_v1_p3_independent": "Strict 1+3",
    "partition_v1_p3_max_union": "Strict 1+3, max-union",
    "partition_v2_p2_independent": "Strict 2+2",
    "partition_v2_p2_max_union": "Strict 2+2, max-union",
    "partition_v3_p1_independent": "Strict 3+1",
    "partition_v3_p1_max_union": "Strict 3+1, max-union",
    "partition_v3_p3_independent": "Strict 3+3",
    "partition_v3_p3_max_union": "Strict 3+3, max-union",
    "reserve_v1_p1_t4_independent": "Reserve 1+1 / T4",
    "reserve_v1_p1_t4_max_union": "Reserve 1+1 / T4, max-union",
    "reserve_v1_p1_t6_independent": "Reserve 1+1 / T6",
    "reserve_v1_p1_t6_max_union": "Reserve 1+1 / T6, max-union",
    "reserve_v2_p2_t6_independent": "Reserve 2+2 / T6",
    "reserve_v2_p2_t6_max_union": "Reserve 2+2 / T6, max-union",
}


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def checkpoint_design(checkpoint: Mapping, name: str, kind: str) -> Mapping:
    if kind == "reference":
        return checkpoint["references"][name]
    return checkpoint["schemes"][name]


def aggregate_design(document: Mapping, name: str, kind: str) -> Mapping:
    if kind == "reference":
        return document["aggregate"]["references"][name]
    return document["aggregate"]["schemes"][name]


def ratio(metrics: Mapping, numerator: str) -> float:
    denominator = metrics["demand_count"]
    return metrics[numerator] / denominator if denominator else 0.0


def sent_per_demand(metrics: Mapping) -> float:
    denominator = metrics["demand_count"]
    sent = sum(metrics["sent_by_role"].values())
    return sent / denominator if denominator else 0.0


def available_designs(
    document: Mapping,
    requested: Iterable[Tuple[str, str, str, str]],
) -> List[Tuple[str, str, str, str]]:
    first = document["checkpoints"][0]
    result = []
    for design in requested:
        name, kind, _, _ = design
        container = first["references"] if kind == "reference" else first["schemes"]
        if name in container:
            result.append(design)
    return result


def all_report_designs(document: Mapping) -> List[Tuple[str, str, str, str]]:
    designs = list(BASE_DESIGNS)
    palette = (
        "#d1495b",
        "#f28e2b",
        "#00a676",
        "#8f5bb7",
        "#4e79a7",
        "#e15759",
    )
    scheme_names = sorted(document["checkpoints"][0]["schemes"])
    for index, name in enumerate(scheme_names):
        designs.append((
            name,
            "scheme",
            SCHEME_LABELS.get(name, name),
            palette[index % len(palette)],
        ))
    return designs


def write_csv_report(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    for checkpoint in document["checkpoints"]:
        for name, kind, label, _ in all_report_designs(document):
            design = checkpoint_design(checkpoint, name, kind)
            rows.append({
                "checkpoint": checkpoint["name"],
                "weight": checkpoint["weight"],
                "design": label,
                "ideal_coverage": design["metrics"]["coverage"],
                "local_coverage": design["local_metrics"]["coverage"],
                "generated_per_demand": ratio(
                    design["metrics"], "generated_count"
                ),
                "sent_per_demand": sent_per_demand(design["local_metrics"]),
            })
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_aggregate_csv(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    for name, kind, label, _ in all_report_designs(document):
        design = aggregate_design(document, name, kind)
        for weighting in ("checkpoint_weighted", "access_weighted"):
            rows.append({
                "design": label,
                "weighting": weighting,
                "ideal_coverage": design["ideal_coverage"][weighting],
                "local_coverage": design["local_coverage"][weighting],
                "generated_per_demand": design["generated_per_demand"][weighting],
                "sent_per_demand": design["local_sent_per_demand"][weighting],
            })
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def axis_bounds(values: Iterable[float], step: float) -> Tuple[float, float]:
    values = list(values)
    minimum = math.floor(min(values) / step) * step
    maximum = math.ceil(max(values) / step) * step
    if minimum == maximum:
        maximum += step
    return minimum, maximum


def render_checkpoint_coverage(
    document: Mapping,
    requested_designs: Iterable[Tuple[str, str, str, str]],
    title: str,
    path: pathlib.Path,
) -> None:
    designs = available_designs(document, requested_designs)
    checkpoints = document["checkpoints"]
    all_values = [
        checkpoint_design(checkpoint, name, kind)["metrics"]["coverage"]
        for checkpoint in checkpoints
        for name, kind, _, _ in designs
    ]
    x_min, x_max = axis_bounds(all_values, 0.1)
    width = 1420
    chart_x = 230
    chart_width = 1135
    top = 105
    row_height = 54
    height = top + row_height * len(checkpoints) + 45
    body = [
        text(24, 30, title, "title"),
        text(
            24,
            50,
            "Static trace replay; each point is address-match ideal coverage for one checkpoint.",
            "subtitle",
        ),
    ]
    legend_x = 24
    for _, _, label, color in designs:
        body.append(rect(legend_x, 65, 13, 13, color))
        body.append(text(legend_x + 19, 76, label, "small"))
        legend_x += 190

    tick = x_min
    while tick <= x_max + 1e-9:
        x = chart_x + (tick - x_min) / (x_max - x_min) * chart_width
        body.append(
            f'<line x1="{x:.1f}" y1="92" x2="{x:.1f}" '
            f'y2="{height - 26}" class="axis"/>'
        )
        body.append(text(x, 90, f"{tick * 100:.0f}%", "small", "middle"))
        tick += 0.1

    point_spacing = min(7.0, 32.0 / max(1, len(designs) - 1))
    for row, checkpoint in enumerate(checkpoints):
        center_y = top + row * row_height + row_height / 2
        body.append(text(18, center_y + 4, checkpoint["name"], "label"))
        body.append(
            f'<line x1="{chart_x:.1f}" y1="{center_y:.1f}" '
            f'x2="{chart_x + chart_width:.1f}" y2="{center_y:.1f}" '
            'stroke="#eef1f5" stroke-width="1"/>'
        )
        for index, (name, kind, _, color) in enumerate(designs):
            coverage = checkpoint_design(checkpoint, name, kind)["metrics"]["coverage"]
            x = chart_x + (coverage - x_min) / (x_max - x_min) * chart_width
            y = center_y + (index - (len(designs) - 1) / 2) * point_spacing
            body.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4.4" '
                f'fill="{color}" stroke="#ffffff" stroke-width="1"/>'
            )
    path.write_text(svg_document(width, height, body))


def render_tradeoff(document: Mapping, path: pathlib.Path) -> None:
    designs = available_designs(document, TRADEOFF_DESIGNS)
    width = 1400
    height = 610
    panel_width = 630
    panel_height = 430
    origins = ((70, 115), (750, 115))
    weighting_modes = (
        ("checkpoint_weighted", "Checkpoint-weighted"),
        ("access_weighted", "Access-weighted"),
    )
    body = [
        text(24, 30, "Partitioned Student pool: local coverage / traffic trade-off", "title"),
        text(
            24,
            50,
            "Coverage and sent/demand after the shared 256-entry filter, Primary 8 / Extra 4 FIFOs, and 3:1 arbiter.",
            "subtitle",
        ),
    ]
    legend_x = 24
    for _, _, label, color in designs:
        body.append(rect(legend_x, 68, 13, 13, color))
        body.append(text(legend_x + 19, 79, label, "small"))
        legend_x += 190

    for (mode, title), (origin_x, origin_y) in zip(weighting_modes, origins):
        points = []
        for name, kind, label, color in designs:
            aggregate = aggregate_design(document, name, kind)
            points.append((
                aggregate["local_sent_per_demand"][mode],
                aggregate["local_coverage"][mode],
                label,
                color,
            ))
        x_min, x_max = axis_bounds((point[0] for point in points), 0.1)
        y_min, y_max = axis_bounds((point[1] for point in points), 0.02)
        plot_x = origin_x + 58
        plot_y = origin_y + 34
        plot_width = panel_width - 88
        plot_height = panel_height - 85
        body.append(text(origin_x, origin_y, title, "label"))
        for tick_index in range(6):
            fraction = tick_index / 5
            x = plot_x + fraction * plot_width
            x_value = x_min + fraction * (x_max - x_min)
            body.append(
                f'<line x1="{x:.1f}" y1="{plot_y:.1f}" x2="{x:.1f}" '
                f'y2="{plot_y + plot_height:.1f}" class="axis"/>'
            )
            body.append(
                text(x, plot_y + plot_height + 20, f"{x_value:.2f}", "small", "middle")
            )
            y = plot_y + plot_height - fraction * plot_height
            y_value = y_min + fraction * (y_max - y_min)
            body.append(
                f'<line x1="{plot_x:.1f}" y1="{y:.1f}" '
                f'x2="{plot_x + plot_width:.1f}" y2="{y:.1f}" class="axis"/>'
            )
            body.append(text(plot_x - 8, y + 4, f"{y_value * 100:.0f}%", "small", "end"))
        body.append(
            text(
                plot_x + plot_width / 2,
                plot_y + plot_height + 42,
                "local sent / demand",
                "small",
                "middle",
            )
        )
        body.append(
            text(plot_x, plot_y - 10, "local coverage", "small")
        )
        for sent, coverage, label, color in points:
            x = plot_x + (sent - x_min) / (x_max - x_min) * plot_width
            y = plot_y + plot_height - (coverage - y_min) / (y_max - y_min) * plot_height
            body.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="6" '
                f'fill="{color}" stroke="#ffffff" stroke-width="1.5"/>'
            )
            offset_x, offset_y = TRADEOFF_LABEL_OFFSETS[mode][label]
            body.append(text(x + offset_x, y + offset_y, label, "small"))
    path.write_text(svg_document(width, height, body))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate tables and SVGs for partitioned TS-BOP replay"
    )
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--out-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    document = load_json(args.input)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_csv_report(
        document,
        args.out_dir / "ts_bop_partitioned_pool_compare.csv",
    )
    write_aggregate_csv(
        document,
        args.out_dir / "ts_bop_partitioned_pool_aggregate.csv",
    )
    render_checkpoint_coverage(
        document,
        POOL4_DESIGNS,
        "Partitioned Student pool: pool=4 ideal coverage",
        args.out_dir / "ts_bop_partitioned_pool_coverage_pool4.svg",
    )
    render_checkpoint_coverage(
        document,
        POOL6_DESIGNS,
        "Partitioned Student pool: pool=6 diagnostic",
        args.out_dir / "ts_bop_partitioned_pool_coverage_pool6.svg",
    )
    render_tradeoff(
        document,
        args.out_dir / "ts_bop_partitioned_pool_tradeoff.svg",
    )


if __name__ == "__main__":
    main()
