#!/usr/bin/env python3

"""Generate review artifacts for the selected TS-BOP family gate."""

import argparse
import csv
import json
import pathlib
import sys
from typing import Dict, Mapping

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.offset_distribution import (  # noqa: E402
    rect,
    svg_document,
    text,
)


DUAL = "dual_current"
BASELINE = "baseline_n2r50s20"
FINAL = "final_v2p0_family_conf_n2r50s20"


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def sent_per_demand(metrics: Mapping) -> float:
    demand = metrics["demand_count"]
    sent = sum(metrics.get("sent_by_role", {}).values())
    return sent / demand if demand else 0.0


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


def write_checkpoint_csv(document: Mapping, path: pathlib.Path) -> None:
    rows = []
    for checkpoint in document["checkpoints"]:
        schemes = checkpoint["schemes"]
        dual = schemes[DUAL]["local_metrics"]
        baseline = schemes[BASELINE]["local_metrics"]
        final_result = schemes[FINAL]
        final_local = final_result["local_metrics"]
        phase = final_result["phase"]
        rows.append({
            "checkpoint": checkpoint["name"],
            "weight": checkpoint["weight"],
            "measurement_accesses": checkpoint["measurement_accesses"],
            "dual_local_coverage": dual["coverage"],
            "baseline_local_coverage": baseline["coverage"],
            "final_ideal_coverage": final_result["metrics"]["coverage"],
            "final_local_coverage": final_local["coverage"],
            "final_minus_dual": final_local["coverage"] - dual["coverage"],
            "final_minus_baseline": (
                final_local["coverage"] - baseline["coverage"]
            ),
            "dual_sent_per_demand": sent_per_demand(dual),
            "baseline_sent_per_demand": sent_per_demand(baseline),
            "final_sent_per_demand": sent_per_demand(final_local),
            "teacher_phase_count": phase["phase_count"],
            "vbop_qualified_fraction": phase["vbop_qualified_fraction"],
            "pbop_qualified_fraction": phase["pbop_qualified_fraction"],
            "vbop_average_best_score": phase["vbop_average_best_score"],
            "pbop_average_best_score": phase["pbop_average_best_score"],
        })
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def render_coverage(document: Mapping, path: pathlib.Path) -> None:
    checkpoints = document["checkpoints"]
    width = 1120
    row_height = 62
    top = 104
    bottom = 48
    height = top + row_height * len(checkpoints) + bottom
    chart_left = 250
    chart_width = 700
    body = [
        text(24, 30, "Final TS-BOP local coverage by checkpoint", "title"),
        text(
            24,
            50,
            "Dual BOP, merged global confidence, and V2/P0 family-independent confidence.",
            "subtitle",
        ),
        rect(620, 20, 16, 10, "#7b8494"),
        text(642, 30, "Dual BOP", "small"),
        rect(735, 20, 16, 10, "#4e79a7"),
        text(757, 30, "Global confidence", "small"),
        rect(900, 20, 16, 10, "#2f9d68"),
        text(922, 30, "Family confidence", "small"),
    ]
    chart_bottom = top + row_height * len(checkpoints)
    for step in range(5):
        coverage = step * 25
        x = chart_left + chart_width * coverage / 100.0
        body.append(line(x, top - 12, x, chart_bottom, "#edf0f5"))
        body.append(text(x, top - 18, f"{coverage}%", "small", "middle"))
    for index, checkpoint in enumerate(checkpoints):
        schemes = checkpoint["schemes"]
        values = (
            schemes[DUAL]["local_metrics"]["coverage"],
            schemes[BASELINE]["local_metrics"]["coverage"],
            schemes[FINAL]["local_metrics"]["coverage"],
        )
        y = top + index * row_height
        body.append(text(24, y + 24, checkpoint["name"], "label"))
        for bar_index, (coverage, color) in enumerate(zip(
            values,
            ("#7b8494", "#4e79a7", "#2f9d68"),
        )):
            body.append(rect(
                chart_left,
                y + bar_index * 13,
                coverage * chart_width,
                9,
                color,
            ))
        delta = (values[2] - values[0]) * 100.0
        body.append(text(
            chart_left + chart_width + 12,
            y + 33,
            f"{values[2] * 100:.2f}% ({delta:+.2f} pp)",
            "small",
        ))
    path.write_text(svg_document(width, height, body))


def render_qualification(document: Mapping, path: pathlib.Path) -> None:
    checkpoints = document["checkpoints"]
    width = 1080
    row_height = 50
    top = 100
    bottom = 46
    height = top + row_height * len(checkpoints) + bottom
    chart_left = 260
    chart_width = 650
    body = [
        text(24, 30, "Teacher family qualification by checkpoint", "title"),
        text(
            24,
            50,
            "Fraction of shared Teacher phases satisfying VBOP score > 2 or PBOP score > 0.",
            "subtitle",
        ),
        rect(715, 20, 16, 10, "#cf5b61"),
        text(737, 30, "VBOP V2", "small"),
        rect(825, 20, 16, 10, "#4e79a7"),
        text(847, 30, "PBOP P0", "small"),
    ]
    chart_bottom = top + row_height * len(checkpoints)
    for step in range(5):
        percent = step * 25
        x = chart_left + chart_width * percent / 100.0
        body.append(line(x, top - 10, x, chart_bottom, "#edf0f5"))
        body.append(text(x, top - 16, f"{percent}%", "small", "middle"))
    for index, checkpoint in enumerate(checkpoints):
        phase = checkpoint["schemes"][FINAL]["phase"]
        vbop = phase["vbop_qualified_fraction"]
        pbop = phase["pbop_qualified_fraction"]
        y = top + index * row_height
        body.append(text(24, y + 20, checkpoint["name"], "label"))
        body.append(rect(chart_left, y, vbop * chart_width, 12, "#cf5b61"))
        body.append(rect(chart_left, y + 17, pbop * chart_width, 12, "#4e79a7"))
        body.append(text(
            chart_left + chart_width + 12,
            y + 11,
            f"V {vbop * 100:.1f}%",
            "small",
        ))
        body.append(text(
            chart_left + chart_width + 12,
            y + 28,
            f"P {pbop * 100:.1f}%",
            "small",
        ))
    path.write_text(svg_document(width, height, body))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Report final TS-BOP family-confidence replay"
    )
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--out-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()

    document = load_json(args.input)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_checkpoint_csv(
        document,
        args.out_dir / "ts_bop_family_confidence_selected_10pt.csv",
    )
    render_coverage(
        document,
        args.out_dir / "ts_bop_family_confidence_checkpoint_coverage.svg",
    )
    render_qualification(
        document,
        args.out_dir / "ts_bop_family_confidence_qualification.svg",
    )


if __name__ == "__main__":
    main()
