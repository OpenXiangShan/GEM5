#!/usr/bin/env python3

import argparse
import csv
import html
import json
import math
import pathlib
import sys
from collections import Counter
from dataclasses import dataclass
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.analyze import PBOP_OFFSETS, VBOP_OFFSETS


DUAL_SCHEME = "dual_current"
D1_SCHEME = "merged_d1_top1"
D2_SCHEME = "merged_d2_greedy_marginal_th2_top1"

STREAM_LABELS = {
    "dual_vbop": "Dual VBOP",
    "dual_pbop": "Dual PBOP",
    "merged_primary": "Merged primary",
    "merged_extra": "Merged extra",
    "dual_total": "Dual combined",
    "merged_total": "Merged D2 combined",
}

VBOP_SET = set(VBOP_OFFSETS)
PBOP_SET = set(PBOP_OFFSETS)

COLORS = {
    "vbop": "#3b82f6",
    "pbop": "#f59e0b",
    "dual_vbop": "#2563eb",
    "dual_pbop": "#d97706",
    "merged_primary": "#059669",
    "merged_extra": "#7c3aed",
    "other": "#98a2b3",
}


@dataclass
class CheckpointData:
    name: str
    weight: float
    demand_count: int
    dual_coverage: float
    merged_coverage: float
    streams: Dict[str, Counter]


def load_json(path: pathlib.Path) -> Dict:
    return json.loads(path.read_text())


def find_scheme(document: Mapping, name: str) -> Mapping:
    return next(scheme for scheme in document["schemes"] if scheme["name"] == name)


def issued_offset_distribution(stats: Mapping[str, int]) -> Counter:
    prefix = "issued_offset::"
    return Counter({
        int(name[len(prefix):]): int(value)
        for name, value in stats.items()
        if name.startswith(prefix) and value
    })


def subtract_distribution(total: Counter, primary: Counter) -> Counter:
    result = Counter()
    for offset in set(total) | set(primary):
        count = total[offset] - primary[offset]
        if count < 0:
            raise ValueError(
                f"primary count exceeds degree-2 total for offset {offset}: "
                f"{primary[offset]} > {total[offset]}"
            )
        if count:
            result[offset] = count
    return result


def load_checkpoint(entry: Mapping) -> CheckpointData:
    result_dir = pathlib.Path(entry["result_dir"])
    shortlist = load_json(result_dir / "shortlist_corrected.json")
    params = load_json(result_dir / "params_top1.json")
    resources = load_json(result_dir / "resources_filter.json")

    dual = find_scheme(shortlist, DUAL_SCHEME)
    d1 = find_scheme(params, D1_SCHEME)
    d2 = find_scheme(resources, D2_SCHEME)

    dual_vbop = issued_offset_distribution(
        dual["replay_stats"]["VBOP"]["measurement"]
    )
    dual_pbop = issued_offset_distribution(
        dual["replay_stats"]["PBOP"]["measurement"]
    )
    merged_primary = issued_offset_distribution(
        d1["replay_stats"]["MergedVBOP"]["measurement"]
    )
    merged_total = issued_offset_distribution(
        d2["replay_stats"]["MergedVBOP"]["measurement"]
    )
    merged_extra = subtract_distribution(merged_total, merged_primary)

    d2_stats = d2["replay_stats"]["MergedVBOP"]["measurement"]
    expected_primary = int(d2_stats.get("generated_rank_0", 0))
    expected_extra = int(d2_stats.get("generated_rank_1", 0))
    if sum(merged_primary.values()) != expected_primary:
        raise ValueError(
            f"{entry['name']}: degree-1 distribution does not match "
            f"degree-2 primary count"
        )
    if sum(merged_extra.values()) != expected_extra:
        raise ValueError(
            f"{entry['name']}: derived extra distribution does not match "
            f"degree-2 extra count"
        )

    streams = {
        "dual_vbop": dual_vbop,
        "dual_pbop": dual_pbop,
        "merged_primary": merged_primary,
        "merged_extra": merged_extra,
        "dual_total": dual_vbop + dual_pbop,
        "merged_total": merged_primary + merged_extra,
    }
    return CheckpointData(
        name=entry["name"],
        weight=float(entry["weight"]),
        demand_count=int(dual["metrics"]["demand_count"]),
        dual_coverage=float(dual["metrics"]["coverage"]),
        merged_coverage=float(d2["metrics"]["coverage"]),
        streams=streams,
    )


def offset_family(offset: int) -> str:
    if offset in VBOP_SET:
        return "vbop"
    if offset in PBOP_SET:
        return "pbop"
    raise ValueError(f"offset {offset} is not in the merged offset list")


def family_share(distribution: Mapping[int, float], family: str) -> float:
    total = sum(distribution.values())
    if not total:
        return 0.0
    return sum(
        count for offset, count in distribution.items()
        if offset_family(offset) == family
    ) / total


def checkpoint_weighted_distribution(
    checkpoints: Sequence[CheckpointData],
    stream: str,
) -> Dict[int, float]:
    weighted = Counter()
    weight_sum = 0.0
    for checkpoint in checkpoints:
        distribution = checkpoint.streams[stream]
        total = sum(distribution.values())
        if not total:
            continue
        weight_sum += checkpoint.weight
        for offset, count in distribution.items():
            weighted[offset] += checkpoint.weight * count / total
    if not weight_sum:
        return {}
    return {
        offset: value / weight_sum
        for offset, value in weighted.items()
    }


def access_weighted_distribution(
    checkpoints: Sequence[CheckpointData],
    stream: str,
) -> Dict[int, float]:
    counts = Counter()
    for checkpoint in checkpoints:
        counts.update(checkpoint.streams[stream])
    total = sum(counts.values())
    if not total:
        return {}
    return {offset: count / total for offset, count in counts.items()}


def checkpoint_weighted_requests_per_demand(
    checkpoints: Sequence[CheckpointData],
    stream: str,
) -> float:
    weight_sum = sum(checkpoint.weight for checkpoint in checkpoints)
    if not weight_sum:
        return 0.0
    return sum(
        checkpoint.weight
        * sum(checkpoint.streams[stream].values())
        / checkpoint.demand_count
        for checkpoint in checkpoints
    ) / weight_sum


def access_weighted_requests_per_demand(
    checkpoints: Sequence[CheckpointData],
    stream: str,
) -> float:
    requests = sum(
        sum(checkpoint.streams[stream].values())
        for checkpoint in checkpoints
    )
    demands = sum(checkpoint.demand_count for checkpoint in checkpoints)
    return requests / demands if demands else 0.0


def percent(value: float) -> str:
    return f"{value * 100:.1f}%"


def offset_label(offset: int) -> str:
    return f"{offset:+d}"


def svg_document(width: int, height: int, body: Iterable[str]) -> str:
    style = "\n".join([
        "<style>",
        "  text { font-family: Arial, sans-serif; fill: #172033; }",
        "  .title { font-size: 20px; font-weight: 500; }",
        "  .subtitle { font-size: 12px; fill: #667085; }",
        "  .label { font-size: 12px; }",
        "  .small { font-size: 11px; fill: #667085; }",
        "  .axis { stroke: #d9dee8; stroke-width: 1; }",
        "</style>",
    ])
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
        f'height="{height}" viewBox="0 0 {width} {height}" role="img">',
        style,
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
    ]
    lines.extend(body)
    lines.append("</svg>")
    return "\n".join(lines) + "\n"


def text(x: float, y: float, value: str, css_class: str = "label", anchor: str = "start") -> str:
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" class="{css_class}" '
        f'text-anchor="{anchor}">{html.escape(value)}</text>'
    )


def rect(x: float, y: float, width: float, height: float, fill: str) -> str:
    return (
        f'<rect x="{x:.1f}" y="{y:.1f}" width="{max(0.0, width):.1f}" '
        f'height="{height:.1f}" fill="{fill}"/>'
    )


def render_family_mix(checkpoints: Sequence[CheckpointData], path: pathlib.Path) -> None:
    width = 1220
    row_height = 58
    height = 122 + row_height * len(checkpoints)
    chart_x = 250
    chart_width = 650
    bar_height = 15
    body = [
        text(24, 30, "Offset-family mix by checkpoint", "title"),
        text(
            24,
            50,
            "BOP-selected candidates before the local recent filter and FIFO; bars are normalized within each design.",
            "subtitle",
        ),
        rect(760, 18, 16, 12, COLORS["vbop"]),
        text(782, 29, "Former VBOP offsets", "small"),
        rect(930, 18, 16, 12, COLORS["pbop"]),
        text(952, 29, "Former PBOP offsets", "small"),
        text(chart_x - 12, 82, "Design", "small", "end"),
        text(chart_x, 82, "0%", "small"),
        text(chart_x + chart_width / 2, 82, "50%", "small", "middle"),
        text(chart_x + chart_width, 82, "100%", "small", "end"),
        text(chart_x + chart_width + 40, 82, "requests / demand", "small"),
    ]
    for fraction in (0.0, 0.5, 1.0):
        x = chart_x + fraction * chart_width
        body.append(
            f'<line x1="{x:.1f}" y1="88" x2="{x:.1f}" '
            f'y2="{height - 18}" class="axis"/>'
        )

    for index, checkpoint in enumerate(checkpoints):
        top = 100 + index * row_height
        body.append(text(18, top + 24, checkpoint.name, "label"))
        rows = (
            ("Dual", checkpoint.streams["dual_total"], top + 4),
            ("Merged D2", checkpoint.streams["merged_total"], top + 26),
        )
        for label, distribution, y in rows:
            vbop_share = family_share(distribution, "vbop")
            pbop_share = family_share(distribution, "pbop")
            requests_per_demand = sum(distribution.values()) / checkpoint.demand_count
            body.append(text(chart_x - 12, y + 12, label, "small", "end"))
            body.append(rect(chart_x, y, chart_width * vbop_share, bar_height, COLORS["vbop"]))
            body.append(
                rect(
                    chart_x + chart_width * vbop_share,
                    y,
                    chart_width * pbop_share,
                    bar_height,
                    COLORS["pbop"],
                )
            )
            body.append(
                text(
                    chart_x + chart_width + 40,
                    y + 12,
                    f"{requests_per_demand:.3f}",
                    "label",
                )
            )
            if vbop_share >= 0.12:
                body.append(
                    text(
                        chart_x + chart_width * vbop_share / 2,
                        y + 12,
                        percent(vbop_share),
                        "small",
                        "middle",
                    )
                )
            if pbop_share >= 0.12:
                body.append(
                    text(
                        chart_x + chart_width * (vbop_share + pbop_share / 2),
                        y + 12,
                        percent(pbop_share),
                        "small",
                        "middle",
                    )
                )
    path.write_text(svg_document(width, height, body))


def render_aggregate_distribution(
    checkpoints: Sequence[CheckpointData],
    path: pathlib.Path,
) -> None:
    width = 1240
    height = 800
    panel_width = 570
    panel_height = 345
    origins = ((30, 82), (640, 82), (30, 430), (640, 430))
    streams = ("dual_vbop", "dual_pbop", "merged_primary", "merged_extra")
    body = [
        text(24, 30, "Checkpoint-weighted offset distribution", "title"),
        text(
            24,
            50,
            (
                "Top offsets within each stream; 'other' combines the "
                "remaining offsets. Color follows the historical offset family."
            ),
            "subtitle",
        ),
    ]
    for stream, (origin_x, origin_y) in zip(streams, origins):
        distribution = checkpoint_weighted_distribution(checkpoints, stream)
        ordered = sorted(distribution.items(), key=lambda item: (-item[1], item[0]))
        top = ordered[:10]
        other = max(0.0, 1.0 - sum(value for _, value in top))
        rows: List[Tuple[str, float, str]] = [
            (offset_label(offset), value, COLORS[offset_family(offset)])
            for offset, value in top
        ]
        if other > 0.00005:
            rows.append(("other", other, COLORS["other"]))
        max_value = max(value for _, value, _ in rows)
        plot_x = origin_x + 72
        plot_width = panel_width - 160
        bar_height = 17
        row_gap = 25
        pbop_share = family_share(distribution, "pbop")
        req = checkpoint_weighted_requests_per_demand(checkpoints, stream)
        body.append(text(origin_x, origin_y, STREAM_LABELS[stream], "label"))
        body.append(
            text(
                origin_x,
                origin_y + 18,
                f"former PBOP share {percent(pbop_share)}; requests/demand {req:.3f}",
                "small",
            )
        )
        for row_index, (label, value, color) in enumerate(rows):
            y = origin_y + 38 + row_index * row_gap
            body.append(text(plot_x - 10, y + 13, label, "label", "end"))
            body.append(rect(plot_x, y, plot_width * value / max_value, bar_height, color))
            body.append(
                text(
                    plot_x + plot_width * value / max_value + 8,
                    y + 13,
                    percent(value),
                    "small",
                )
            )
    path.write_text(svg_document(width, height, body))


def nice_axis_max(value: float) -> float:
    if value <= 0:
        return 1.0
    candidates = (0.1, 0.2, 0.25, 0.5, 0.75, 1.0)
    for candidate in candidates:
        if value <= candidate:
            return candidate
    return math.ceil(value * 10) / 10


def render_gap_points(checkpoints: Sequence[CheckpointData], path: pathlib.Path) -> None:
    selected = sorted(
        checkpoints,
        key=lambda checkpoint: checkpoint.merged_coverage - checkpoint.dual_coverage,
    )[:3]
    width = 1320
    row_height = 310
    height = 82 + row_height * len(selected)
    body = [
        text(24, 30, "Offset distributions at the three largest degree-2 coverage gaps", "title"),
        text(
            24,
            50,
            (
                "Grouped bars show within-stream request share. Only the ten "
                "most prominent offsets at each checkpoint are shown."
            ),
            "subtitle",
        ),
    ]
    legend_x = 680
    for index, stream in enumerate(("dual_vbop", "dual_pbop", "merged_primary", "merged_extra")):
        x = legend_x + index * 150
        body.append(rect(x, 19, 13, 11, COLORS[stream]))
        body.append(text(x + 19, 29, STREAM_LABELS[stream], "small"))

    for checkpoint_index, checkpoint in enumerate(selected):
        top = 76 + checkpoint_index * row_height
        normalized: Dict[str, Dict[int, float]] = {}
        prominence = Counter()
        for stream in ("dual_vbop", "dual_pbop", "merged_primary", "merged_extra"):
            distribution = checkpoint.streams[stream]
            total = sum(distribution.values())
            normalized[stream] = {
                offset: count / total for offset, count in distribution.items()
            } if total else {}
            for offset, share in normalized[stream].items():
                prominence[offset] = max(prominence[offset], share)
        offsets = [
            offset for offset, _ in sorted(
                prominence.items(), key=lambda item: (-item[1], item[0])
            )[:10]
        ]
        offsets.sort()
        max_share = max(
            (normalized[stream].get(offset, 0.0)
             for stream in normalized for offset in offsets),
            default=1.0,
        )
        axis_max = nice_axis_max(max_share)
        chart_x = 90
        chart_y = top + 42
        chart_width = 1180
        chart_height = 205
        body.append(text(24, top + 16, checkpoint.name, "label"))
        body.append(
            text(
                220,
                top + 16,
                f"address ideal gap {(checkpoint.merged_coverage - checkpoint.dual_coverage) * 100:+.2f} pp",
                "small",
            )
        )
        for fraction in (0.0, 0.5, 1.0):
            y = chart_y + chart_height * (1.0 - fraction)
            body.append(
                f'<line x1="{chart_x}" y1="{y:.1f}" '
                f'x2="{chart_x + chart_width}" y2="{y:.1f}" class="axis"/>'
            )
            body.append(
                text(
                    chart_x - 10,
                    y + 4,
                    percent(axis_max * fraction),
                    "small",
                    "end",
                )
            )
        group_width = chart_width / max(1, len(offsets))
        bar_width = min(20.0, group_width / 5.2)
        streams = ("dual_vbop", "dual_pbop", "merged_primary", "merged_extra")
        for offset_index, offset in enumerate(offsets):
            center = chart_x + group_width * (offset_index + 0.5)
            for stream_index, stream in enumerate(streams):
                share = normalized[stream].get(offset, 0.0)
                bar_h = chart_height * share / axis_max
                x = center + (stream_index - 1.5) * bar_width
                body.append(
                    rect(
                        x,
                        chart_y + chart_height - bar_h,
                        bar_width - 1.5,
                        bar_h,
                        COLORS[stream],
                    )
                )
            body.append(
                text(center, chart_y + chart_height + 20, offset_label(offset), "small", "middle")
            )
    path.write_text(svg_document(width, height, body))


def checkpoint_to_dict(checkpoint: CheckpointData) -> Dict:
    streams = {}
    for stream, distribution in checkpoint.streams.items():
        total = sum(distribution.values())
        streams[stream] = {
            "request_count": total,
            "requests_per_demand": total / checkpoint.demand_count,
            "former_vbop_share": family_share(distribution, "vbop"),
            "former_pbop_share": family_share(distribution, "pbop"),
            "offset_counts": dict(sorted(distribution.items())),
        }
    return {
        "name": checkpoint.name,
        "weight": checkpoint.weight,
        "demand_count": checkpoint.demand_count,
        "dual_coverage": checkpoint.dual_coverage,
        "merged_coverage": checkpoint.merged_coverage,
        "streams": streams,
    }


def aggregate_to_dict(checkpoints: Sequence[CheckpointData]) -> Dict:
    result = {}
    for stream in STREAM_LABELS:
        checkpoint_weighted = checkpoint_weighted_distribution(checkpoints, stream)
        access_weighted = access_weighted_distribution(checkpoints, stream)
        result[stream] = {
            "checkpoint_weighted_requests_per_demand": (
                checkpoint_weighted_requests_per_demand(checkpoints, stream)
            ),
            "access_weighted_requests_per_demand": (
                access_weighted_requests_per_demand(checkpoints, stream)
            ),
            "checkpoint_weighted_former_vbop_share": family_share(
                checkpoint_weighted, "vbop"
            ),
            "checkpoint_weighted_former_pbop_share": family_share(
                checkpoint_weighted, "pbop"
            ),
            "access_weighted_former_vbop_share": family_share(
                access_weighted, "vbop"
            ),
            "access_weighted_former_pbop_share": family_share(
                access_weighted, "pbop"
            ),
            "checkpoint_weighted_offset_share": dict(sorted(checkpoint_weighted.items())),
            "access_weighted_offset_share": dict(sorted(access_weighted.items())),
        }
    return result


def write_csv(path: pathlib.Path, checkpoints: Sequence[CheckpointData]) -> None:
    with path.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow([
            "checkpoint",
            "weight",
            "demand_count",
            "stream",
            "offset",
            "offset_family",
            "request_count",
            "within_stream_share",
            "requests_per_demand",
        ])
        for checkpoint in checkpoints:
            for stream, distribution in checkpoint.streams.items():
                total = sum(distribution.values())
                for offset, count in sorted(distribution.items()):
                    writer.writerow([
                        checkpoint.name,
                        checkpoint.weight,
                        checkpoint.demand_count,
                        stream,
                        offset,
                        offset_family(offset),
                        count,
                        count / total if total else 0.0,
                        count / checkpoint.demand_count,
                    ])


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Summarize and plot TS-BOP offset distributions"
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("util/ts_bop_degree/checkpoints.json"),
    )
    parser.add_argument(
        "--out-dir",
        type=pathlib.Path,
        default=pathlib.Path("docs/design-docs/images"),
    )
    args = parser.parse_args()

    manifest = load_json(args.manifest)
    checkpoints = [load_checkpoint(entry) for entry in manifest]
    args.out_dir.mkdir(parents=True, exist_ok=True)

    result = {
        "manifest": str(args.manifest),
        "definition": (
            "BOP-selected candidate offsets before the local recent filter, "
            "FIFO, parent PFQ, and cache-state effects"
        ),
        "checkpoints": [checkpoint_to_dict(item) for item in checkpoints],
        "aggregate": aggregate_to_dict(checkpoints),
    }
    (args.out_dir / "ts_bop_offset_distribution.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    write_csv(args.out_dir / "ts_bop_offset_distribution.csv", checkpoints)
    render_family_mix(
        checkpoints,
        args.out_dir / "ts_bop_offset_family_mix.svg",
    )
    render_aggregate_distribution(
        checkpoints,
        args.out_dir / "ts_bop_offset_distribution_aggregate.svg",
    )
    render_gap_points(
        checkpoints,
        args.out_dir / "ts_bop_offset_distribution_gap_points.svg",
    )


if __name__ == "__main__":
    main()
