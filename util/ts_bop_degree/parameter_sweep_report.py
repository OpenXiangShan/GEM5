#!/usr/bin/env python3

import argparse
import csv
import json
import pathlib
import sys
from typing import Dict, Iterable, Mapping, Sequence

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.offset_distribution import (  # noqa: E402
    rect,
    svg_document,
    text,
)


DUAL = "dual_current"
POOL2 = "s20_v2p2_sf2048_e8_rf128"
POOL4 = "s20_v4p4_sf2048_e8_rf128"
FILTER_SCHEMES = (
    (0, "s20_v4p4_sf2048_e8_rf0"),
    (32, "s20_v4p4_sf2048_e8_rf32"),
    (64, "s20_v4p4_sf2048_e8_rf64"),
    (128, "s20_v4p4_sf2048_e8_rf128"),
    (256, "s20_v4p4_sf2048_e8_rf256"),
)


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def sent_per_demand(metrics: Mapping) -> float:
    demand = metrics["demand_count"]
    sent = sum(metrics.get("sent_by_role", {}).values())
    return sent / demand if demand else 0.0


def write_checkpoint_csv(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    for checkpoint in document["checkpoints"]:
        schemes = checkpoint["schemes"]
        dual = schemes[DUAL]["local_metrics"]
        pool2 = schemes[POOL2]["local_metrics"]
        pool4 = schemes[POOL4]["local_metrics"]
        rows.append({
            "checkpoint": checkpoint["name"],
            "weight": checkpoint["weight"],
            "measurement_accesses": checkpoint["measurement_accesses"],
            "dual_local_coverage": dual["coverage"],
            "pool2_local_coverage": pool2["coverage"],
            "pool4_local_coverage": pool4["coverage"],
            "pool4_minus_dual": pool4["coverage"] - dual["coverage"],
            "pool4_minus_pool2": pool4["coverage"] - pool2["coverage"],
            "dual_sent_per_demand": sent_per_demand(dual),
            "pool2_sent_per_demand": sent_per_demand(pool2),
            "pool4_sent_per_demand": sent_per_demand(pool4),
        })
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def line(
    x1: float,
    y1: float,
    x2: float,
    y2: float,
    stroke: str,
    width: float = 1.0,
) -> str:
    return (
        f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" '
        f'y2="{y2:.1f}" stroke="{stroke}" stroke-width="{width:.1f}"/>'
    )


def circle(x: float, y: float, radius: float, fill: str, stroke: str) -> str:
    return (
        f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{radius:.1f}" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="1.5"/>'
    )


def render_checkpoint_delta(document: Mapping, path: pathlib.Path) -> None:
    checkpoints = document["checkpoints"]
    width = 1040
    row_height = 38
    height = 92 + row_height * len(checkpoints)
    chart_left = 270
    chart_width = 650
    zero_x = chart_left + chart_width * 0.82
    positive_limit = 4.0
    negative_limit = 20.0
    negative_width = zero_x - chart_left
    positive_width = chart_left + chart_width - zero_x
    body = [
        text(24, 30, "Selected TS-BOP local coverage delta", "title"),
        text(
            24,
            50,
            "N2/R50/S20, Student 4+4, filter 2048, P8/E8, recent filter 128; delta versus dual BOP.",
            "subtitle",
        ),
        line(zero_x, 67, zero_x, height - 24, "#7b8494", 1.2),
        text(zero_x, 65, "0 pp", "small", "middle"),
    ]
    for index, checkpoint in enumerate(checkpoints):
        y = 82 + index * row_height
        schemes = checkpoint["schemes"]
        dual = schemes[DUAL]["local_metrics"]["coverage"]
        selected = schemes[POOL4]["local_metrics"]["coverage"]
        delta = (selected - dual) * 100.0
        body.append(text(24, y + 12, checkpoint["name"], "label"))
        if delta >= 0:
            bar_width = min(positive_width, delta / positive_limit * positive_width)
            body.append(rect(zero_x, y, bar_width, 17, "#2f9d68"))
            label_x = zero_x + bar_width + 7
            anchor = "start"
        else:
            bar_width = min(negative_width, -delta / negative_limit * negative_width)
            body.append(rect(zero_x - bar_width, y, bar_width, 17, "#cf5b61"))
            label_x = zero_x - bar_width - 7
            anchor = "end"
        body.append(text(label_x, y + 13, f"{delta:+.2f} pp", "small", anchor))
    path.write_text(svg_document(width, height, body))


def render_filter_tradeoff(document: Mapping, path: pathlib.Path) -> None:
    width = 900
    height = 520
    left = 90
    right = 52
    top = 76
    bottom = 72
    chart_width = width - left - right
    chart_height = height - top - bottom
    values = []
    for entries, name in FILTER_SCHEMES:
        aggregate = document["aggregate"][name]
        values.append((
            entries,
            aggregate["local_sent_per_demand"]["access_weighted"],
            aggregate["local_coverage"]["access_weighted"] * 100.0,
        ))
    x_min = min(value[1] for value in values) - 0.025
    x_max = max(value[1] for value in values) + 0.025
    y_min = min(value[2] for value in values) - 0.04
    y_max = max(value[2] for value in values) + 0.04

    def x_pos(value: float) -> float:
        return left + (value - x_min) / (x_max - x_min) * chart_width

    def y_pos(value: float) -> float:
        return top + (y_max - value) / (y_max - y_min) * chart_height

    body = [
        text(24, 30, "Recent-filter coverage and traffic trade-off", "title"),
        text(
            24,
            50,
            "Six-checkpoint sensitivity; access-weighted local coverage versus locally sent requests per demand.",
            "subtitle",
        ),
        line(left, top + chart_height, left + chart_width, top + chart_height, "#aeb6c4"),
        line(left, top, left, top + chart_height, "#aeb6c4"),
    ]
    for step in range(5):
        fraction = step / 4
        x_value = x_min + fraction * (x_max - x_min)
        x = x_pos(x_value)
        body.append(line(x, top, x, top + chart_height, "#edf0f5"))
        body.append(text(x, top + chart_height + 24, f"{x_value:.2f}", "small", "middle"))
        y_value = y_min + fraction * (y_max - y_min)
        y = y_pos(y_value)
        body.append(line(left, y, left + chart_width, y, "#edf0f5"))
        body.append(text(left - 10, y + 4, f"{y_value:.2f}%", "small", "end"))
    body.append(text(left + chart_width / 2, height - 22, "Local sent / demand", "label", "middle"))
    body.append(
        f'<text x="20" y="{top + chart_height / 2:.1f}" class="label" '
        f'text-anchor="middle" transform="rotate(-90 20 {top + chart_height / 2:.1f})">'
        "Local coverage</text>"
    )
    for entries, sent, coverage in values:
        selected = entries == 128
        fill = "#2f9d68" if selected else "#4e79a7"
        radius = 7 if selected else 5
        x = x_pos(sent)
        y = y_pos(coverage)
        body.append(circle(x, y, radius, fill, "#ffffff"))
        label = "off" if entries == 0 else str(entries)
        body.append(text(x + 9, y - 8, label, "small"))
    path.write_text(svg_document(width, height, body))

def write_candidate_comparison_csv(
    candidate: Mapping,
    reference: Mapping,
    candidate_scheme: str,
    reference_scheme: str,
    path: pathlib.Path,
) -> None:
    reference_by_name = {
        checkpoint["name"]: checkpoint
        for checkpoint in reference["checkpoints"]
    }
    rows = []
    for checkpoint in candidate["checkpoints"]:
        name = checkpoint["name"]
        if name not in reference_by_name:
            raise ValueError(f"reference is missing checkpoint {name}")
        reference_checkpoint = reference_by_name[name]
        if (
            checkpoint["measurement_accesses"]
            != reference_checkpoint["measurement_accesses"]
        ):
            raise ValueError(f"measurement window differs for {name}")
        dual = checkpoint["schemes"][DUAL]["local_metrics"]
        candidate_result = checkpoint["schemes"][candidate_scheme]
        candidate_local = candidate_result["local_metrics"]
        reference_local = reference_checkpoint["schemes"][reference_scheme][
            "local_metrics"
        ]
        phase = candidate_result.get("phase", {})
        rows.append({
            "checkpoint": name,
            "weight": checkpoint["weight"],
            "measurement_accesses": checkpoint["measurement_accesses"],
            "dual_local_coverage": dual["coverage"],
            "reference_local_coverage": reference_local["coverage"],
            "candidate_ideal_coverage": candidate_result["metrics"]["coverage"],
            "candidate_local_coverage": candidate_local["coverage"],
            "candidate_minus_dual": (
                candidate_local["coverage"] - dual["coverage"]
            ),
            "candidate_minus_reference": (
                candidate_local["coverage"] - reference_local["coverage"]
            ),
            "dual_sent_per_demand": sent_per_demand(dual),
            "reference_sent_per_demand": sent_per_demand(reference_local),
            "candidate_sent_per_demand": sent_per_demand(candidate_local),
            "candidate_phase_count": phase.get("phase_count", 0),
            "candidate_average_best_score": phase.get(
                "average_best_score", 0.0
            ),
            "candidate_issue_enabled_fraction": phase.get(
                "issue_enabled_fraction", 0.0
            ),
            "candidate_two_family_winner_fraction": phase.get(
                "two_family_winner_fraction", 0.0
            ),
        })
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def render_candidate_coverage(
    candidate: Mapping,
    reference: Mapping,
    candidate_scheme: str,
    reference_scheme: str,
    candidate_label: str,
    reference_label: str,
    path: pathlib.Path,
) -> None:
    reference_by_name = {
        checkpoint["name"]: checkpoint
        for checkpoint in reference["checkpoints"]
    }
    checkpoints = candidate["checkpoints"]
    width = 1080
    row_height = 61
    top = 104
    bottom = 48
    height = top + row_height * len(checkpoints) + bottom
    chart_left = 250
    chart_width = 700
    body = [
        text(24, 30, "TS-BOP local coverage by checkpoint", "title"),
        text(
            24,
            50,
            f"Dual BOP versus {reference_label} and {candidate_label}; same trace windows.",
            "subtitle",
        ),
        rect(650, 20, 16, 10, "#7b8494"),
        text(672, 30, "Dual BOP", "small"),
        rect(765, 20, 16, 10, "#4e79a7"),
        text(787, 30, reference_label, "small"),
        rect(910, 20, 16, 10, "#cf5b61"),
        text(932, 30, candidate_label, "small"),
    ]
    chart_bottom = top + row_height * len(checkpoints)
    for step in range(5):
        coverage = step * 25
        x = chart_left + chart_width * coverage / 100.0
        body.append(line(x, top - 12, x, chart_bottom, "#edf0f5"))
        body.append(text(x, top - 18, f"{coverage}%", "small", "middle"))
    for index, checkpoint in enumerate(checkpoints):
        name = checkpoint["name"]
        reference_checkpoint = reference_by_name[name]
        dual = checkpoint["schemes"][DUAL]["local_metrics"]["coverage"]
        reference_coverage = reference_checkpoint["schemes"][reference_scheme][
            "local_metrics"
        ]["coverage"]
        candidate_coverage = checkpoint["schemes"][candidate_scheme][
            "local_metrics"
        ]["coverage"]
        y = top + index * row_height
        body.append(text(24, y + 24, name, "label"))
        for bar_index, (coverage, color) in enumerate((
            (dual, "#7b8494"),
            (reference_coverage, "#4e79a7"),
            (candidate_coverage, "#cf5b61"),
        )):
            body.append(rect(
                chart_left,
                y + bar_index * 13,
                coverage * chart_width,
                9,
                color,
            ))
        delta = (candidate_coverage - dual) * 100.0
        body.append(text(
            chart_left + chart_width + 12,
            y + 33,
            f"{candidate_coverage * 100:.2f}% ({delta:+.2f} pp)",
            "small",
        ))
    path.write_text(svg_document(width, height, body))


def main() -> None:
    parser = argparse.ArgumentParser(description="Report selected TS-BOP parameter sweep")
    parser.add_argument("--selected", required=True, type=pathlib.Path)
    parser.add_argument("--resources", required=True, type=pathlib.Path)
    parser.add_argument("--out-dir", required=True, type=pathlib.Path)
    parser.add_argument("--candidate", type=pathlib.Path)
    parser.add_argument("--candidate-scheme")
    parser.add_argument("--reference-scheme")
    parser.add_argument("--candidate-label", default="Candidate")
    parser.add_argument("--reference-label", default="Reference")
    parser.add_argument("--comparison-prefix", default="ts_bop_candidate")
    args = parser.parse_args()
    comparison_args = (
        args.candidate,
        args.candidate_scheme,
        args.reference_scheme,
    )
    if any(comparison_args) and not all(comparison_args):
        parser.error(
            "--candidate, --candidate-scheme, and --reference-scheme "
            "must be provided together"
        )

    selected = load_json(args.selected)
    resources = load_json(args.resources)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_checkpoint_csv(
        selected,
        args.out_dir / "ts_bop_r50_s20_selected_10pt.csv",
    )
    render_checkpoint_delta(
        selected,
        args.out_dir / "ts_bop_r50_s20_selected_checkpoint_delta.svg",
    )
    render_filter_tradeoff(
        resources,
        args.out_dir / "ts_bop_recent_filter_tradeoff.svg",
    )
    if args.candidate:
        candidate = load_json(args.candidate)
        write_candidate_comparison_csv(
            candidate,
            selected,
            args.candidate_scheme,
            args.reference_scheme,
            args.out_dir / f"{args.comparison_prefix}_selected_10pt.csv",
        )
        render_candidate_coverage(
            candidate,
            selected,
            args.candidate_scheme,
            args.reference_scheme,
            args.candidate_label,
            args.reference_label,
            args.out_dir / (
                f"{args.comparison_prefix}_checkpoint_coverage.svg"
            ),
        )



if __name__ == "__main__":
    main()
