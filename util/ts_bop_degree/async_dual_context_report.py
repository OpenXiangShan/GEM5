#!/usr/bin/env python3

import argparse
import csv
import html
import json
import pathlib
from typing import Dict, List, Mapping, Sequence, Tuple


SCHEMES = (
    ("dual", "reference", "Dual BOP", "#333333"),
    ("merged_baseline", "reference", "Merged D2 R50/S31", "#9c755f"),
    ("async_v2_p2", "scheme", "Async 2+2", "#d1495b"),
    (
        "async_v2_p2_pbop_delay",
        "scheme",
        "Async 2+2, PBOP delay",
        "#b07aa1",
    ),
    ("async_v2_p8", "scheme", "Async 2+8", "#4e79a7"),
    ("async_v3_p3", "scheme", "Async 3+3", "#f28e2b"),
    ("async_v4_p2", "scheme", "Async 4+2", "#76b7b2"),
    ("async_v4_p8", "scheme", "Async 4+8", "#59a14f"),
)


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def metric_for_checkpoint(
    checkpoint: Mapping,
    name: str,
    kind: str,
) -> Tuple[Mapping, Mapping]:
    if kind == "reference":
        item = checkpoint["references"][name]
    else:
        item = checkpoint["schemes"][name]
    return item["metrics"], item["local_metrics"]


def metric_for_aggregate(
    document: Mapping,
    name: str,
    kind: str,
) -> Mapping:
    group = "references" if kind == "reference" else "schemes"
    return document["aggregate"][group][name]


def sent_per_demand(local: Mapping) -> float:
    return sum(local["sent_by_role"].values()) / local["demand_count"]


def write_aggregate_csv(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    dual = metric_for_aggregate(document, "dual", "reference")
    for name, kind, label, _ in SCHEMES:
        aggregate = metric_for_aggregate(document, name, kind)
        for weighting in ("checkpoint_weighted", "access_weighted"):
            ideal = aggregate["ideal_coverage"][weighting]
            local = aggregate["local_coverage"][weighting]
            rows.append({
                "design": label,
                "name": name,
                "kind": kind,
                "weighting": weighting,
                "ideal_coverage": ideal,
                "local_coverage": local,
                "generated_per_demand": aggregate[
                    "generated_per_demand"
                ][weighting],
                "local_sent_per_demand": aggregate[
                    "local_sent_per_demand"
                ][weighting],
                "ideal_delta_dual": (
                    ideal - dual["ideal_coverage"][weighting]
                ),
                "local_delta_dual": (
                    local - dual["local_coverage"][weighting]
                ),
            })
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_checkpoint_csv(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    for checkpoint in document["checkpoints"]:
        dual_ideal, dual_local = metric_for_checkpoint(
            checkpoint,
            "dual",
            "reference",
        )
        for name, kind, label, _ in SCHEMES:
            ideal, local = metric_for_checkpoint(checkpoint, name, kind)
            rows.append({
                "checkpoint": checkpoint["name"],
                "workload": checkpoint["workload"],
                "weight": checkpoint["weight"],
                "design": label,
                "name": name,
                "ideal_coverage": ideal["coverage"],
                "local_coverage": local["coverage"],
                "generated_per_demand": (
                    ideal["generated_count"] / ideal["demand_count"]
                ),
                "local_sent_per_demand": sent_per_demand(local),
                "ideal_delta_dual": (
                    ideal["coverage"] - dual_ideal["coverage"]
                ),
                "local_delta_dual": (
                    local["coverage"] - dual_local["coverage"]
                ),
            })
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def svg_text(
    x: float,
    y: float,
    value: str,
    css_class: str = "",
    anchor: str = "start",
) -> str:
    attrs = f' class="{css_class}"' if css_class else ""
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}"'
        f'{attrs}>{html.escape(value)}</text>'
    )


def svg_header(width: int, height: int) -> List[str]:
    return [
        '<?xml version="1.0" encoding="UTF-8"?>',
        (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
            f'height="{height}" viewBox="0 0 {width} {height}">'
        ),
        "<style>",
        "text { font-family: Arial, sans-serif; fill: #202124; }",
        ".title { font-size: 18px; font-weight: 700; }",
        ".subtitle { font-size: 12px; fill: #5f6368; }",
        ".label { font-size: 11px; }",
        ".small { font-size: 10px; fill: #5f6368; }",
        ".axis { stroke: #80868b; stroke-width: 1; }",
        ".grid { stroke: #e0e0e0; stroke-width: 1; }",
        "</style>",
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
    ]


def render_checkpoint_delta(document: Mapping, path: pathlib.Path) -> None:
    shown = (
        ("async_v2_p2", "Async 2+2", "#d1495b"),
        (
            "async_v2_p2_pbop_delay",
            "Async 2+2, PBOP delay",
            "#b07aa1",
        ),
        ("async_v4_p8", "Async 4+8", "#59a14f"),
    )
    checkpoints = document["checkpoints"]
    width = 1120
    left = 190
    right = 35
    top = 105
    row_height = 58
    height = top + len(checkpoints) * row_height + 55
    plot_width = width - left - right
    values = []
    rows = []
    for checkpoint in checkpoints:
        dual = checkpoint["references"]["dual"]["local_metrics"]["coverage"]
        row = []
        for name, label, color in shown:
            value = (
                checkpoint["schemes"][name]["local_metrics"]["coverage"]
                - dual
            ) * 100.0
            values.append(value)
            row.append((label, color, value))
        rows.append(row)
    bound = max(1.0, max(abs(value) for value in values) * 1.12)
    x0 = left + plot_width / 2
    scale = plot_width / (2 * bound)

    output = svg_header(width, height)
    output.append(svg_text(24, 30, "Async dual-context local coverage delta", "title"))
    output.append(svg_text(
        24,
        50,
        "Percentage-point delta versus original dual BOP; target local resource is F256 P8/E4 3:1.",
        "subtitle",
    ))
    legend_x = 24
    for _, label, color in shown:
        output.append(
            f'<rect x="{legend_x}" y="67" width="12" height="12" fill="{color}"/>'
        )
        output.append(svg_text(legend_x + 17, 77, label, "label"))
        legend_x += 205

    for tick in range(-4, 5):
        value = bound * tick / 4
        x = x0 + value * scale
        output.append(
            f'<line x1="{x:.1f}" y1="{top - 10}" x2="{x:.1f}" '
            f'y2="{height - 35}" class="{("axis" if tick == 0 else "grid")}"/>'
        )
        output.append(svg_text(x, height - 17, f"{value:+.1f}", "small", "middle"))

    bar_height = 12
    for row_index, checkpoint in enumerate(checkpoints):
        center_y = top + row_index * row_height + row_height / 2
        output.append(svg_text(left - 12, center_y + 4, checkpoint["name"], "label", "end"))
        for item_index, (_, color, value) in enumerate(rows[row_index]):
            y = center_y - 20 + item_index * 15
            x1 = x0
            x2 = x0 + value * scale
            output.append(
                f'<rect x="{min(x1, x2):.1f}" y="{y:.1f}" '
                f'width="{max(1.0, abs(x2 - x1)):.1f}" height="{bar_height}" '
                f'fill="{color}"/>'
            )
            anchor = "start" if value >= 0 else "end"
            text_x = x2 + (4 if value >= 0 else -4)
            output.append(svg_text(text_x, y + 10, f"{value:+.2f}", "small", anchor))
    output.append("</svg>")
    path.write_text("\n".join(output) + "\n")


def render_tradeoff(document: Mapping, path: pathlib.Path) -> None:
    width = 1160
    height = 545
    panel_width = 500
    panel_height = 340
    top = 135
    lefts = (75, 620)
    weightings = (
        ("checkpoint_weighted", "Checkpoint-weighted"),
        ("access_weighted", "Access-weighted"),
    )
    points: Dict[str, List[Tuple[str, str, float, float]]] = {}
    for weighting, _ in weightings:
        current = []
        for name, kind, label, color in SCHEMES:
            aggregate = metric_for_aggregate(document, name, kind)
            current.append((
                label,
                color,
                aggregate["local_sent_per_demand"][weighting],
                aggregate["local_coverage"][weighting] * 100.0,
            ))
        points[weighting] = current
    all_x = [item[2] for values in points.values() for item in values]
    all_y = [item[3] for values in points.values() for item in values]
    x_min = min(all_x) - 0.04
    x_max = max(all_x) + 0.04
    y_min = min(all_y) - 1.0
    y_max = max(all_y) + 1.0

    output = svg_header(width, height)
    output.append(svg_text(24, 30, "Async dual-context coverage / traffic trade-off", "title"))
    output.append(svg_text(
        24,
        50,
        "Upper-left is preferable: higher local demand coverage with fewer candidates leaving the local FIFO.",
        "subtitle",
    ))
    for index, (_, _, label, color) in enumerate(SCHEMES):
        column = index % 4
        row = index // 4
        x = 24 + column * 270
        y = 68 + row * 20
        output.append(
            f'<circle cx="{x + 5}" cy="{y - 4}" r="5" fill="{color}"/>'
        )
        output.append(svg_text(x + 16, y, label, "small"))
    for panel, (weighting, title) in enumerate(weightings):
        left = lefts[panel]
        bottom = top + panel_height
        output.append(svg_text(left, 120, title, "label"))
        output.append(
            f'<line x1="{left}" y1="{bottom}" x2="{left + panel_width}" '
            f'y2="{bottom}" class="axis"/>'
        )
        output.append(
            f'<line x1="{left}" y1="{top}" x2="{left}" y2="{bottom}" class="axis"/>'
        )
        for tick in range(6):
            x_value = x_min + (x_max - x_min) * tick / 5
            x = left + panel_width * tick / 5
            output.append(
                f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" '
                f'y2="{bottom}" class="grid"/>'
            )
            output.append(svg_text(x, bottom + 20, f"{x_value:.2f}", "small", "middle"))
            y_value = y_min + (y_max - y_min) * tick / 5
            y = bottom - panel_height * tick / 5
            output.append(
                f'<line x1="{left}" y1="{y:.1f}" x2="{left + panel_width}" '
                f'y2="{y:.1f}" class="grid"/>'
            )
            output.append(svg_text(left - 8, y + 4, f"{y_value:.1f}%", "small", "end"))
        output.append(svg_text(
            left + panel_width / 2,
            bottom + 42,
            "local sent / demand",
            "label",
            "middle",
        ))
        for index, (label, color, x_value, y_value) in enumerate(points[weighting]):
            x = left + (x_value - x_min) / (x_max - x_min) * panel_width
            y = bottom - (y_value - y_min) / (y_max - y_min) * panel_height
            output.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="{color}"/>'
            )
    output.append("</svg>")
    path.write_text("\n".join(output) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate async dual-context TS-BOP tables and SVGs"
    )
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument(
        "--out-dir",
        type=pathlib.Path,
        default=pathlib.Path("docs/design-docs/images"),
    )
    args = parser.parse_args()
    document = load_json(args.input)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_aggregate_csv(
        document,
        args.out_dir / "ts_bop_async_dual_context_aggregate.csv",
    )
    write_checkpoint_csv(
        document,
        args.out_dir / "ts_bop_async_dual_context_checkpoint.csv",
    )
    render_checkpoint_delta(
        document,
        args.out_dir / "ts_bop_async_dual_context_checkpoint_delta.svg",
    )
    render_tradeoff(
        document,
        args.out_dir / "ts_bop_async_dual_context_tradeoff.svg",
    )


if __name__ == "__main__":
    main()
