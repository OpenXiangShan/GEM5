#!/usr/bin/env python3
"""Join matched LVP A/B performance with saturated-value profiling.

The performance side is deliberately restricted to the already matched
IdealConstantLVP-enabled/disabled runs (run947/run102 in the current study).
The profiling side is independent v3 output: no profiling cycles, IPC, score,
or speedup is consumed here.  A complete value profile is required before any
decision table or chart is emitted.

Inputs:
  --ab-dir       directory containing ``per_slice_comparison.csv`` and
                 ``sensitive_slices.csv``
  --value-dir    output directory from ``ideal_constant_lvp_values.py``
  --out-dir      destination for joined CSV, charts, and a Markdown fragment

The generated Markdown fragment uses relative image links so it can be
included directly from a report stored beside ``out-dir``.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence


VALUE_VERSION = "ideal_constant_lvp_saturated_values_v1"
REQUIRED_AB_FIELDS = {
    "slice",
    "benchmark",
    "cycle_speedup_pct",
    "enabled_cycles",
    "disabled_cycles",
}
REQUIRED_VALUE_SLICE_FIELDS = {
    "slice",
    "benchmark",
    "scope",
    "profile_version",
    "pc_entries",
    "cumulative_distinct_values",
    "global_distinct_saturated_values",
    "value_sharing_saved_slots",
    "prediction_use_columns_present",
    "prediction_uses",
    "correct_prediction_uses",
    "stats_vp_supported",
    "stats_vp_predicted",
    "stats_vp_corrected",
    "coverage_contribution_pct",
    "concurrent_distinct_value_peak",
    "concurrent_distinct_value_peak_source",
    "interval_concurrent_distinct_value_peak",
    "interval_concurrent_saturated_pc_peak",
}
REQUIRED_VALUE_PC_FIELDS = {
    "slice",
    "benchmark",
    "scope",
    "tid",
    "pc",
    "distinct_saturated_values",
    "saturated_value_segments",
    "prediction_uses",
    "correct_prediction_uses",
    "wrong_prediction_uses",
    "coverage_contribution_pct",
}


class DecisionError(ValueError):
    """Inputs do not satisfy the complete A/B plus v3 contract."""


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ab-dir", type=Path, required=True)
    parser.add_argument("--value-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--expected-slices", type=int, default=None)
    parser.add_argument("--skip-charts", action="store_true")
    return parser.parse_args(argv)


def read_csv(path: Path) -> List[Dict[str, str]]:
    try:
        with path.open(newline="", errors="replace") as source:
            reader = csv.DictReader(source)
            if reader.fieldnames is None:
                raise DecisionError(f"{path} has no CSV header")
            return list(reader)
    except OSError:
        raise


def require_fields(path: Path, rows: Sequence[Mapping[str, str]], fields: Iterable[str]) -> None:
    if not rows:
        raise DecisionError(f"{path} is empty")
    actual = set(rows[0])
    missing = sorted(set(fields) - actual)
    if missing:
        raise DecisionError(f"{path} is missing fields: {missing}")
    for index, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise DecisionError(f"{path}:{index} has the wrong number of columns")


def indexed(
    rows: Sequence[Mapping[str, str]], key: str, description: str
) -> Dict[str, Mapping[str, str]]:
    result: Dict[str, Mapping[str, str]] = {}
    for row in rows:
        item_key = row[key]
        if item_key in result:
            raise DecisionError(f"duplicate {description} key {item_key!r}")
        result[item_key] = row
    return result


def as_int(value: Optional[str], field: str, allow_empty: bool = False) -> Optional[int]:
    if value is None or value == "":
        if allow_empty:
            return None
        raise DecisionError(f"missing integer field {field}")
    try:
        parsed = int(value, 0)
    except (TypeError, ValueError) as error:
        try:
            numeric = float(value)
        except (TypeError, ValueError):
            raise DecisionError(f"invalid integer {field}={value!r}") from error
        if not math.isfinite(numeric) or not numeric.is_integer():
            raise DecisionError(f"invalid integer {field}={value!r}")
        parsed = int(numeric)
    return parsed


def as_float(value: Optional[str], field: str, allow_empty: bool = False) -> Optional[float]:
    if value is None or value == "":
        if allow_empty:
            return None
        raise DecisionError(f"missing numeric field {field}")
    try:
        parsed = float(value)
    except (TypeError, ValueError) as error:
        raise DecisionError(f"invalid numeric {field}={value!r}") from error
    if not math.isfinite(parsed):
        raise DecisionError(f"non-finite numeric {field}={value!r}")
    return parsed


def percentile(values: Iterable[float], percentage: float) -> Optional[float]:
    ordered = sorted(value for value in values if math.isfinite(value))
    if not ordered:
        return None
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * percentage / 100.0) - 1))
    return ordered[index]


def distribution(values: Iterable[float]) -> Dict[str, Optional[float]]:
    finite = [value for value in values if math.isfinite(value)]
    return {
        "count": len(finite),
        "p50": percentile(finite, 50),
        "p90": percentile(finite, 90),
        "p95": percentile(finite, 95),
        "p99": percentile(finite, 99),
        "max": max(finite) if finite else None,
    }


def benchmark_groups(rows: Sequence[Mapping[str, str]]) -> Dict[str, List[Mapping[str, str]]]:
    groups: Dict[str, List[Mapping[str, str]]] = defaultdict(list)
    for row in rows:
        groups[row["benchmark"]].append(row)
    return groups


def sensitivity_for(ab_row: Mapping[str, str], sensitive: Mapping[str, Mapping[str, str]]) -> str:
    return sensitive.get(ab_row["slice"], {}).get("sensitivity_class", "insensitive")


def validate_value_slice(row: Mapping[str, str], path: Path) -> None:
    if row["scope"] != "roi":
        raise DecisionError(f"{path}: expected ROI value summary, got {row['scope']!r}")
    if row["profile_version"] != VALUE_VERSION:
        raise DecisionError(
            f"{path}: unsupported profile version {row['profile_version']!r}"
        )
    if row["concurrent_distinct_value_peak_source"] != "stats":
        raise DecisionError(
            f"{path}: online distinct-value peak is not backed by stats"
        )
    if as_int(row["prediction_use_columns_present"], "prediction_use_columns_present") != 1:
        raise DecisionError(f"{path}: prediction-use columns are incomplete")
    integer_fields = (
        "pc_entries",
        "cumulative_distinct_values",
        "global_distinct_saturated_values",
        "value_sharing_saved_slots",
        "prediction_uses",
        "correct_prediction_uses",
        "stats_vp_supported",
        "stats_vp_predicted",
        "stats_vp_corrected",
        "concurrent_distinct_value_peak",
        "interval_concurrent_distinct_value_peak",
        "interval_concurrent_saturated_pc_peak",
    )
    parsed = {field: as_int(row[field], field) for field in integer_fields}
    if any(value is not None and value < 0 for value in parsed.values()):
        raise DecisionError(f"{path}: negative value in ROI summary")
    if parsed["global_distinct_saturated_values"] > parsed["cumulative_distinct_values"]:
        raise DecisionError(f"{path}: global distinct values exceed cumulative values")
    if parsed["value_sharing_saved_slots"] != (
        parsed["cumulative_distinct_values"]
        - parsed["global_distinct_saturated_values"]
    ):
        raise DecisionError(f"{path}: inconsistent value-sharing saved-slot count")
    if parsed["correct_prediction_uses"] > parsed["prediction_uses"]:
        raise DecisionError(f"{path}: correct uses exceed prediction uses")
    if parsed["stats_vp_predicted"] != parsed["prediction_uses"]:
        raise DecisionError(f"{path}: value CSV uses differ from VPpredicted")
    if parsed["stats_vp_corrected"] != parsed["correct_prediction_uses"]:
        raise DecisionError(f"{path}: value CSV correct uses differ from VPcorrected")
    if parsed["stats_vp_supported"] < parsed["correct_prediction_uses"]:
        raise DecisionError(f"{path}: correct uses exceed VPsupported")
    as_float(row["coverage_contribution_pct"], "coverage_contribution_pct")


def validate_value_pc(rows: Sequence[Mapping[str, str]], path: Path) -> None:
    require_fields(path, rows, REQUIRED_VALUE_PC_FIELDS)
    seen = set()
    for index, row in enumerate(rows, start=2):
        if row["scope"] != "roi":
            raise DecisionError(f"{path}:{index}: expected ROI per-PC row")
        key = (row["slice"], as_int(row["tid"], "tid"), row["pc"])
        if key in seen:
            raise DecisionError(f"{path}:{index}: duplicate per-PC key {key!r}")
        seen.add(key)
        distinct = as_int(row["distinct_saturated_values"], "distinct_saturated_values")
        segments = as_int(row["saturated_value_segments"], "saturated_value_segments")
        uses = as_int(row["prediction_uses"], "prediction_uses")
        correct = as_int(row["correct_prediction_uses"], "correct_prediction_uses")
        wrong = as_int(row["wrong_prediction_uses"], "wrong_prediction_uses")
        if min(distinct, segments, uses, correct, wrong) < 0:
            raise DecisionError(f"{path}:{index}: negative per-PC value")
        if correct > uses or wrong != uses - correct:
            raise DecisionError(f"{path}:{index}: inconsistent per-PC use counts")
        as_float(row["coverage_contribution_pct"], "coverage_contribution_pct")


def pc_distribution_for(
    rows: Sequence[Mapping[str, str]], slice_name: str
) -> Dict[str, Optional[float]]:
    values = [
        float(as_int(row["distinct_saturated_values"], "distinct_saturated_values"))
        for row in rows
        if row["slice"] == slice_name
    ]
    result = distribution(values)
    result["multi_value_pcs"] = sum(value > 1 for value in values)
    return result


def join_rows(
    ab_rows: Sequence[Mapping[str, str]],
    sensitive_rows: Sequence[Mapping[str, str]],
    value_rows: Sequence[Mapping[str, str]],
    pc_rows: Sequence[Mapping[str, str]],
    ab_path: Path,
    value_path: Path,
    expected_slices: Optional[int],
) -> tuple[List[Dict[str, object]], List[Dict[str, object]], Dict[str, Mapping[str, str]]]:
    require_fields(ab_path, ab_rows, REQUIRED_AB_FIELDS)
    require_fields(value_path, value_rows, REQUIRED_VALUE_SLICE_FIELDS)
    validate_value_pc(pc_rows, value_path.with_name("per_pc_values.csv"))
    ab = indexed(ab_rows, "slice", "A/B slice")
    values = indexed(value_rows, "slice", "value slice")
    sensitive = indexed(sensitive_rows, "slice", "sensitive slice") if sensitive_rows else {}
    if set(ab) != set(values):
        raise DecisionError(
            "A/B and value slice sets differ: "
            f"ab-only={len(set(ab) - set(values))}, "
            f"value-only={len(set(values) - set(ab))}"
        )
    if expected_slices is not None and len(ab) != expected_slices:
        raise DecisionError(f"expected {expected_slices} slices, found {len(ab)}")
    pc_by_slice: Dict[str, List[Mapping[str, str]]] = defaultdict(list)
    for row in pc_rows:
        pc_by_slice[row["slice"]].append(row)
    joined: List[Dict[str, object]] = []
    for slice_name in sorted(ab):
        value = values[slice_name]
        validate_value_slice(value, value_path / "per_slice_values.csv")
        pcs = pc_by_slice.get(slice_name, [])
        cumulative_from_pc = sum(
            as_int(row["distinct_saturated_values"], "distinct_saturated_values")
            for row in pcs
        )
        cumulative = as_int(
            value["cumulative_distinct_values"], "cumulative_distinct_values"
        )
        if cumulative_from_pc != cumulative:
            raise DecisionError(
                f"{slice_name}: per-PC values do not sum to cumulative distinct values"
            )
        profile_pcs = as_int(value["pc_entries"], "pc_entries")
        if profile_pcs != len(pcs):
            raise DecisionError(f"{slice_name}: pc_entries does not match per-PC rows")
        pc_dist = pc_distribution_for(pc_rows, slice_name)
        ab_row = ab[slice_name]
        joined.append(
            {
                "slice": slice_name,
                "benchmark": ab_row["benchmark"],
                "cycle_speedup_pct": as_float(ab_row["cycle_speedup_pct"], "cycle_speedup_pct"),
                "enabled_cycles": as_float(ab_row["enabled_cycles"], "enabled_cycles"),
                "disabled_cycles": as_float(ab_row["disabled_cycles"], "disabled_cycles"),
                "sensitivity_class": sensitivity_for(ab_row, sensitive),
                "profile_pc_entries": profile_pcs,
                "cumulative_distinct_values": cumulative,
                "global_distinct_saturated_values": as_int(
                    value["global_distinct_saturated_values"],
                    "global_distinct_saturated_values",
                ),
                "value_sharing_saved_slots": as_int(
                    value["value_sharing_saved_slots"], "value_sharing_saved_slots"
                ),
                "value_sharing_ratio_pct": (
                    as_int(
                        value["value_sharing_saved_slots"],
                        "value_sharing_saved_slots",
                    )
                    / cumulative
                    * 100.0
                    if cumulative
                    else 0.0
                ),
                "online_peak_distinct_values": as_int(
                    value["concurrent_distinct_value_peak"],
                    "concurrent_distinct_value_peak",
                ),
                "interval_peak_distinct_values": as_int(
                    value["interval_concurrent_distinct_value_peak"],
                    "interval_concurrent_distinct_value_peak",
                    True,
                ),
                "interval_peak_saturated_pcs": as_int(
                    value["interval_concurrent_saturated_pc_peak"],
                    "interval_concurrent_saturated_pc_peak",
                    True,
                ),
                "prediction_uses": as_int(
                    value["prediction_uses"], "prediction_uses"
                ),
                "correct_prediction_uses": as_int(
                    value["correct_prediction_uses"], "correct_prediction_uses"
                ),
                "supported_insts": as_int(
                    value["stats_vp_supported"], "stats_vp_supported"
                ),
                "coverage_contribution_pct": as_float(
                    value["coverage_contribution_pct"],
                    "coverage_contribution_pct",
                ),
                "pc_distinct_values_p50": pc_dist["p50"],
                "pc_distinct_values_p90": pc_dist["p90"],
                "pc_distinct_values_p95": pc_dist["p95"],
                "pc_distinct_values_p99": pc_dist["p99"],
                "pc_distinct_values_max": pc_dist["max"],
                "multi_value_pcs": pc_dist["multi_value_pcs"],
            }
        )
    return joined, [dict(row) for row in pc_rows], sensitive


SLICE_FIELDS = (
    "slice",
    "benchmark",
    "cycle_speedup_pct",
    "enabled_cycles",
    "disabled_cycles",
    "sensitivity_class",
    "profile_pc_entries",
    "cumulative_distinct_values",
    "global_distinct_saturated_values",
    "value_sharing_saved_slots",
    "value_sharing_ratio_pct",
    "online_peak_distinct_values",
    "interval_peak_distinct_values",
    "interval_peak_saturated_pcs",
    "prediction_uses",
    "correct_prediction_uses",
    "supported_insts",
    "coverage_contribution_pct",
    "pc_distinct_values_p50",
    "pc_distinct_values_p90",
    "pc_distinct_values_p95",
    "pc_distinct_values_p99",
    "pc_distinct_values_max",
    "multi_value_pcs",
)
PC_FIELDS = (
    "slice",
    "benchmark",
    "sensitivity_class",
    "tid",
    "pc",
    "distinct_saturated_values",
    "saturated_value_segments",
    "prediction_uses",
    "correct_prediction_uses",
    "wrong_prediction_uses",
    "coverage_contribution_pct",
)


def write_csv(path: Path, rows: Sequence[Mapping[str, object]], fields: Sequence[str]) -> None:
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(fields), extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def group_summary(name: str, rows: Sequence[Mapping[str, object]]) -> Dict[str, object]:
    def nums(field: str) -> List[float]:
        return [float(row[field]) for row in rows if row.get(field) not in (None, "")]

    total_enabled = sum(float(row.get("enabled_cycles", 0) or 0) for row in rows)
    total_disabled = sum(float(row.get("disabled_cycles", 0) or 0) for row in rows)
    result: Dict[str, object] = {
        "group": name,
        "slices": len(rows),
        "weighted_cycle_speedup_pct": (
            (total_disabled - total_enabled) / total_disabled * 100.0
            if total_disabled
            else None
        ),
        "weighted_coverage_contribution_pct": (
            sum(int(row["correct_prediction_uses"]) for row in rows)
            / sum(int(row["supported_insts"]) for row in rows)
            * 100.0
            if sum(int(row["supported_insts"]) for row in rows)
            else None
        ),
    }
    for field in (
        "cumulative_distinct_values",
        "global_distinct_saturated_values",
        "online_peak_distinct_values",
        "interval_peak_distinct_values",
        "interval_peak_saturated_pcs",
        "pc_distinct_values_p95",
        "pc_distinct_values_max",
    ):
        values = nums(field)
        result[f"{field}_p50"] = percentile(values, 50)
        result[f"{field}_p90"] = percentile(values, 90)
        result[f"{field}_p95"] = percentile(values, 95)
        result[f"{field}_p99"] = percentile(values, 99)
        result[f"{field}_max"] = max(values) if values else None
    return result


def workload_summary(rows: Sequence[Mapping[str, object]]) -> List[Dict[str, object]]:
    result = []
    for benchmark, items in sorted(benchmark_groups(rows).items()):
        summary = group_summary(benchmark, items)
        summary["benchmark"] = benchmark
        summary["sensitive_slices"] = sum(
            item["sensitivity_class"] != "insensitive" for item in items
        )
        summary["sensitive_gain_slices"] = sum(
            item["sensitivity_class"] == "gain" for item in items
        )
        summary["sensitive_regression_slices"] = sum(
            item["sensitivity_class"] == "regression" for item in items
        )
        result.append(summary)
    return result


def write_charts(
    out_dir: Path,
    rows: Sequence[Mapping[str, object]],
    pc_rows: Sequence[Mapping[str, object]],
) -> List[str]:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise DecisionError(
            "matplotlib is required for charts; use --skip-charts to omit them"
        ) from error

    plt.rcParams.update({"figure.dpi": 120, "savefig.dpi": 180})
    colors = {"insensitive": "#9aa0a6", "gain": "#2a9d8f", "regression": "#e76f51"}
    labels = {
        "insensitive": "other slices",
        "gain": "sensitive gain",
        "regression": "sensitive regression",
    }
    stems: List[str] = []

    def save(fig, stem: str) -> None:
        fig.tight_layout()
        fig.savefig(out_dir / f"{stem}.png", bbox_inches="tight")
        fig.savefig(out_dir / f"{stem}.svg", bbox_inches="tight")
        plt.close(fig)
        stems.append(stem)

    groups = (
        ("all", list(rows)),
        ("sensitive gain", [row for row in rows if row["sensitivity_class"] == "gain"]),
        ("sensitive regression", [row for row in rows if row["sensitivity_class"] == "regression"]),
    )
    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    for axis, field, title in (
        (axes[0][0], "online_peak_distinct_values", "Online distinct-value peak"),
        (axes[0][1], "cumulative_distinct_values", "Cumulative per-PC distinct values"),
        (axes[1][0], "global_distinct_saturated_values", "Global shared distinct values"),
        (axes[1][1], "pc_distinct_values_p95", "Per-PC distinct-value P95"),
    ):
        data = [
            [float(item[field]) for item in items if item[field] not in (None, "")]
            for _, items in groups
        ]
        axis.boxplot(
            data,
            tick_labels=[f"{name}\nn={len(items)}" for name, items in groups],
            showfliers=False,
        )
        axis.set_title(title)
        axis.set_ylabel("values per slice")
        axis.grid(axis="y", alpha=0.25)
    save(fig, "value_capacity_distributions")

    fig, ax = plt.subplots(figsize=(9, 6))
    for group in ("insensitive", "gain", "regression"):
        items = [
            row for row in rows
            if row["sensitivity_class"] == group
            and float(row["online_peak_distinct_values"]) > 0
        ]
        ax.scatter(
            [float(row["online_peak_distinct_values"]) for row in items],
            [float(row["cycle_speedup_pct"]) for row in items],
            s=24,
            alpha=0.62,
            edgecolors="none",
            color=colors[group],
            label=labels[group],
        )
    ax.set_xscale("log")
    ax.axhline(0, color="#333333", linewidth=0.8)
    ax.set_xlabel("Online distinct saturated values (log)")
    ax.set_ylabel("Matched A/B slice cycle speedup (%)")
    ax.set_title("Speedup versus online value-register demand")
    ax.legend()
    ax.grid(alpha=0.2)
    save(fig, "speedup_vs_online_distinct_value_peak")

    fig, ax = plt.subplots(figsize=(9, 6))
    pc_groups = {
        "all": pc_rows,
        "sensitive gain": [row for row in pc_rows if row.get("sensitivity_class") == "gain"],
        "sensitive regression": [
            row for row in pc_rows if row.get("sensitivity_class") == "regression"
        ],
    }
    for name, items in pc_groups.items():
        values = sorted(float(row["distinct_saturated_values"]) for row in items)
        if not values:
            continue
        ax.step(
            values,
            [(index + 1) / len(values) * 100.0 for index in range(len(values))],
            where="post",
            label=name,
        )
    ax.set_xscale("log")
    ax.set_xlabel("Distinct raw saturated values per (tid, PC) (log)")
    ax.set_ylabel("PCs at or below value (%)")
    ax.set_title("Per-PC raw constant-value diversity")
    ax.legend()
    ax.grid(alpha=0.2)
    save(fig, "per_pc_distinct_value_ecdf")

    fanout_rows = []
    # Fanout is present in per_value_sharing.csv when the caller elects to
    # copy it beside the joined tables; this chart is therefore omitted when
    # that optional file is absent rather than fabricating a proxy.
    sharing_path = out_dir / "per_value_sharing.csv"
    if sharing_path.is_file():
        sharing = read_csv(sharing_path)
        if sharing:
            require_fields(sharing_path, sharing, {"sharing_fanout"})
            fanout_rows = [
                as_int(row["sharing_fanout"], "sharing_fanout") for row in sharing
            ]
    if fanout_rows:
        fig, ax = plt.subplots(figsize=(9, 6))
        ax.hist(
            fanout_rows,
            bins=max(1, min(40, len(set(fanout_rows)))),
            color="#457b9d",
            edgecolor="white",
        )
        ax.set_xlabel("PC owners per shared raw value")
        ax.set_ylabel("Distinct values")
        ax.set_title("Raw-value sharing fanout")
        ax.grid(axis="y", alpha=0.2)
        save(fig, "raw_value_sharing_fanout")
    return stems


def write_markdown(
    out_dir: Path,
    rows: Sequence[Mapping[str, object]],
    charts: Sequence[str],
) -> None:
    all_summary = group_summary("all_slices", rows)
    sensitive = [
        row for row in rows if row["sensitivity_class"] != "insensitive"
    ]
    sensitive_summary = group_summary("sensitive_slices", sensitive)
    lines = [
        "## Raw saturated-value capacity (v3 profiling)",
        "",
        "本节只把 v3 profiling 的 raw `RegVal` 统计与固定的 "
        "run947/run102 A/B speedup 连接；v3 profiling 的 cycles、IPC、score "
        "不参与性能结论。在线容量使用 "
        "`profile*PeakDistinctSaturatedValues`，区间 sweep 仅作为辅助时间边界。",
        "",
        "| 集合 | 切片 | online distinct-value peak P50/P95/max | "
        "cumulative per-PC distinct P50/P95/max | "
        "global shared distinct P50/P95/max |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for name, summary in (("全部", all_summary), ("敏感", sensitive_summary)):
        def format_summary(field: str) -> str:
            value = summary[field]
            return f"{float(value):.0f}" if value is not None else "N/A"

        lines.append(
            f"| {name} | {int(summary['slices'])} | "
            f"{format_summary('online_peak_distinct_values_p50')} / "
            f"{format_summary('online_peak_distinct_values_p95')} / "
            f"{format_summary('online_peak_distinct_values_max')} | "
            f"{format_summary('cumulative_distinct_values_p50')} / "
            f"{format_summary('cumulative_distinct_values_p95')} / "
            f"{format_summary('cumulative_distinct_values_max')} | "
            f"{format_summary('global_distinct_saturated_values_p50')} / "
            f"{format_summary('global_distinct_saturated_values_p95')} / "
            f"{format_summary('global_distinct_saturated_values_max')} |"
        )
    lines.extend(
        [
            "",
            "`cumulative_distinct_values` 是每个 `(tid, PC)` 各占一个 "
            "raw-value slot 的需求；`global_distinct_saturated_values` "
            "对同一切片内相同 raw value 做共享后的下界。不同切片不可相加。",
            "",
        ]
    )
    for chart in charts:
        lines.append(f"![{chart}]({chart}.png)")
        lines.append("")
    (out_dir / "value_decision_report_section.md").write_text("\n".join(lines))


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    try:
        ab_rows = read_csv(args.ab_dir / "per_slice_comparison.csv")
        sensitive_path = args.ab_dir / "sensitive_slices.csv"
        sensitive_rows = read_csv(sensitive_path) if sensitive_path.is_file() else []
        value_rows = read_csv(args.value_dir / "per_slice_values.csv")
        pc_rows_raw = read_csv(args.value_dir / "per_pc_values.csv")
        joined, pc_rows, sensitive = join_rows(
            ab_rows,
            sensitive_rows,
            value_rows,
            pc_rows_raw,
            args.ab_dir / "per_slice_comparison.csv",
            args.value_dir,
            args.expected_slices,
        )
        benchmark_by_slice = {row["slice"]: row["benchmark"] for row in joined}
        for row in pc_rows:
            row["sensitivity_class"] = sensitive.get(row["slice"], {}).get(
                "sensitivity_class", "insensitive"
            )
            row["benchmark"] = benchmark_by_slice[row["slice"]]
    except (OSError, DecisionError, KeyError, ValueError, StopIteration) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.out_dir / "per_slice_value_decision.csv", joined, SLICE_FIELDS)
    write_csv(args.out_dir / "per_pc_value_decision.csv", pc_rows, PC_FIELDS)
    workload_rows = workload_summary(joined)
    if workload_rows:
        write_csv(
            args.out_dir / "per_workload_value_decision.csv",
            workload_rows,
            list(workload_rows[0]),
        )
    charts: List[str] = []
    if not args.skip_charts:
        try:
            # Copy the optional sharing table next to the joined output only
            # when supplied by the value aggregator.
            sharing_source = args.value_dir / "per_value_sharing.csv"
            if sharing_source.is_file():
                sharing_rows = read_csv(sharing_source)
                write_csv(
                    args.out_dir / "per_value_sharing.csv",
                    sharing_rows,
                    list(sharing_rows[0]) if sharing_rows else [],
                )
            charts = write_charts(args.out_dir, joined, pc_rows)
        except (DecisionError, OSError, KeyError, ValueError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
    write_markdown(args.out_dir, joined, charts)
    summary = {
        "joined_slices": len(joined),
        "sensitive_slices": sum(row["sensitivity_class"] != "insensitive" for row in joined),
        "profile_version": VALUE_VERSION,
        "a_b_source": str(args.ab_dir),
        "profiling_source": str(args.value_dir),
        "charts": charts,
        "online_peak_distinct_values": distribution(
            float(row["online_peak_distinct_values"]) for row in joined
        ),
        "cumulative_distinct_values": distribution(
            float(row["cumulative_distinct_values"]) for row in joined
        ),
        "global_distinct_saturated_values": distribution(
            float(row["global_distinct_saturated_values"]) for row in joined
        ),
    }
    (args.out_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n"
    )
    print(json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
