#!/usr/bin/env python3

import argparse
import json
import pathlib
import re
import sys
from typing import Dict, Iterable, List, Optional, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from util.ts_bop_degree.analyze import parse_stats_blocks


SCHEMES = {
    "dual": "dual_current",
    "d1": "merged_d1_top1",
    "d2_raw_t5": "merged_d2_raw_rank_th5_top1",
    "d2_greedy_t0": "merged_d2_greedy_marginal_th0_top1",
    "d2_greedy_t2": "merged_d2_greedy_marginal_th2_top1",
    "d2_greedy_t3": "merged_d2_greedy_marginal_th3_top1",
    "d2_greedy_t4": "merged_d2_greedy_marginal_th4_top1",
    "d2_greedy_t5": "merged_d2_greedy_marginal_th5_top1",
    "d2_greedy_t10": "merged_d2_greedy_marginal_th10_top1",
    "d3_raw_t5": "merged_d3_raw_rank_th5_top1",
    "d3_greedy_t0": "merged_d3_greedy_marginal_th0_top1",
    "d3_greedy_t2": "merged_d3_greedy_marginal_th2_top1",
    "d3_greedy_t5": "merged_d3_greedy_marginal_th5_top1",
    "d3_greedy_t10": "merged_d3_greedy_marginal_th10_top1",
}

TOP2_PAIRS = {
    "d1": ("merged_d1_top1", "merged_d1_top2"),
    "d2_raw_t5": (
        "merged_d2_raw_rank_th5_top1",
        "merged_d2_raw_rank_th5_top2",
    ),
    "d2_greedy_t5": (
        "merged_d2_greedy_marginal_th5_top1",
        "merged_d2_greedy_marginal_th5_top2",
    ),
    "d3_raw_t5": (
        "merged_d3_raw_rank_th5_top1",
        "merged_d3_raw_rank_th5_top2",
    ),
    "d3_greedy_t5": (
        "merged_d3_greedy_marginal_th5_top1",
        "merged_d3_greedy_marginal_th5_top2",
    ),
}

RESOURCE_TARGETS = {
    "dual_p16e16_strict": (SCHEMES["dual"], "dual_p16e16_strict"),
    "t2_p8e4_2to1": (SCHEMES["d2_greedy_t2"], "p8e4_2to1"),
    "t2_p8e4_3to1": (SCHEMES["d2_greedy_t2"], "p8e4_3to1"),
    "t2_p8e4_4to1": (SCHEMES["d2_greedy_t2"], "p8e4_4to1"),
    "t2_p8e8_2to1": (SCHEMES["d2_greedy_t2"], "p8e8_2to1"),
    "t2_p8e8_3to1": (SCHEMES["d2_greedy_t2"], "p8e8_3to1"),
    "t2_p8e8_4to1": (SCHEMES["d2_greedy_t2"], "p8e8_4to1"),
    "t2_f256_p8e4_3to1": (
        SCHEMES["d2_greedy_t2"],
        "f256_p8e4_3to1",
    ),
    "t2_f256_p8e8_3to1": (
        SCHEMES["d2_greedy_t2"],
        "f256_p8e8_3to1",
    ),
    "t3_f256_p8e4_3to1": (
        SCHEMES["d2_greedy_t3"],
        "f256_p8e4_3to1",
    ),
    "t3_f256_p8e8_3to1": (
        SCHEMES["d2_greedy_t3"],
        "f256_p8e8_3to1",
    ),
    "t5_p8e4_3to1": (SCHEMES["d2_greedy_t5"], "p8e4_3to1"),
    "t5_p8e8_3to1": (SCHEMES["d2_greedy_t5"], "p8e8_3to1"),
    "t5_f256_p8e4_3to1": (
        SCHEMES["d2_greedy_t5"],
        "f256_p8e4_3to1",
    ),
    "t5_f256_p8e8_3to1": (
        SCHEMES["d2_greedy_t5"],
        "f256_p8e8_3to1",
    ),
}


def load_optional(path: pathlib.Path) -> Optional[Dict]:
    if not path.exists():
        return None
    return json.loads(path.read_text())


def scheme_map(results: Iterable[Optional[Dict]]) -> Dict[str, Dict]:
    schemes: Dict[str, Dict] = {}
    for result in results:
        if result is None:
            continue
        for scheme in result.get("schemes", []):
            previous = schemes.get(scheme["name"])
            merged = dict(scheme)
            if previous is not None:
                resources = {
                    item["name"]: item
                    for item in previous.get("resources", [])
                }
                resources.update({
                    item["name"]: item
                    for item in merged.get("resources", [])
                })
                merged["resources"] = list(resources.values())
            schemes[scheme["name"]] = merged
    return schemes


def replay_distribution_mismatches(
    result: Dict,
    stats_path: pathlib.Path,
) -> List[Dict]:
    blocks = parse_stats_blocks(stats_path)
    if len(blocks) < 2:
        return [{"error": "stats file has fewer than two blocks"}]
    dual = next(
        (scheme for scheme in result["schemes"] if scheme["name"] == "dual_current"),
        None,
    )
    if dual is None:
        return [{"error": "dual_current replay is missing"}]

    mismatches: List[Dict] = []
    distributions = {
        "issued_offset": ("issuedOffsetDist", "issued_offset::"),
        "student_selected_offset": (
            "studentSelectedOffsetDist",
            "student_selected_offset::",
        ),
        "teacher_injected_offset": (
            "teacherInjectedOffsetDist",
            "teacher_injected_offset::",
        ),
    }
    for block_index, interval in enumerate(("warmup", "measurement")):
        for bop, stat_prefix in (("VBOP", "bop_large"), ("PBOP", "bop_small")):
            replay_interval = dual["replay_stats"][bop][interval]
            for field, (gem5_dist, replay_prefix) in distributions.items():
                gem5_prefix = (
                    f"system.l2_wrappers.prefetcher.{stat_prefix}."
                    f"{gem5_dist}::"
                )
                gem5_bins = {
                    int(name[len(gem5_prefix):]): int(round(value))
                    for name, value in blocks[block_index].items()
                    if name.startswith(gem5_prefix)
                    and re.fullmatch(r"-?\d+", name[len(gem5_prefix):])
                    and value
                }
                replay_bins = {
                    int(name[len(replay_prefix):]): int(value)
                    for name, value in replay_interval.items()
                    if name.startswith(replay_prefix) and value
                }
                if gem5_bins != replay_bins:
                    mismatches.append({
                        "interval": interval,
                        "bop": bop,
                        "field": field,
                        "gem5": gem5_bins,
                        "replay": replay_bins,
                    })
    return mismatches


def coverage(schemes: Dict[str, Dict], name: str) -> Optional[float]:
    scheme = schemes.get(name)
    return None if scheme is None else scheme["metrics"]["coverage"]


def metric_ratio(scheme: Dict, numerator: str) -> float:
    metrics = scheme["metrics"]
    demands = metrics["demand_count"]
    return metrics[numerator] / demands if demands else 0.0


def actual_bop_traffic(
    stats_path: pathlib.Path,
    demand_count: int,
) -> Optional[Dict]:
    blocks = parse_stats_blocks(stats_path)
    if len(blocks) < 2:
        return None
    stats = blocks[1]
    prefix = "system.l2_wrappers.prefetcher."
    counts = {
        "generated": int(round(
            stats.get(f"{prefix}bop_large.pfGenerated", 0)
            + stats.get(f"{prefix}bop_small.pfGenerated", 0)
        )),
        "dequeued": int(round(
            stats.get(f"{prefix}pfDequeued_srcs::HWP_BOP", 0)
        )),
        "issued": int(round(
            stats.get(f"{prefix}pfIssued_srcs::HWP_BOP", 0)
        )),
        "removed_full": int(round(
            stats.get(f"{prefix}pfRemovedFull_srcs::HWP_BOP", 0)
        )),
    }
    return {
        "demand_count": demand_count,
        **{f"{name}_count": count for name, count in counts.items()},
        **{
            f"{name}_per_demand": count / demand_count
            if demand_count else 0.0
            for name, count in counts.items()
        },
    }


def aggregate_actual_bop_traffic(rows: List[Dict]) -> Optional[Dict]:
    selected = [
        row for row in rows
        if row["validated"] and row["actual_bop_traffic"] is not None
    ]
    if not selected:
        return None
    weight_sum = sum(row["weight"] for row in selected)
    total_demands = sum(
        row["actual_bop_traffic"]["demand_count"] for row in selected
    )
    result = {"checkpoint_count": len(selected)}
    for name in ("generated", "dequeued", "issued", "removed_full"):
        result[f"checkpoint_weighted_{name}_per_demand"] = sum(
            row["weight"]
            * row["actual_bop_traffic"][f"{name}_per_demand"]
            for row in selected
        ) / weight_sum
        result[f"access_weighted_{name}_per_demand"] = (
            sum(
                row["actual_bop_traffic"][f"{name}_count"]
                for row in selected
            ) / total_demands
            if total_demands else 0.0
        )
    return result


def aggregate_scheme(rows: List[Dict], scheme_name: str) -> Optional[Dict]:
    selected = [
        row for row in rows
        if row["validated"] and scheme_name in row["schemes"]
    ]
    if not selected:
        return None
    weight_sum = sum(row["weight"] for row in selected)
    total_demands = sum(
        row["schemes"][scheme_name]["metrics"]["demand_count"]
        for row in selected
    )
    total_covered = sum(
        row["schemes"][scheme_name]["metrics"]["covered_count"]
        for row in selected
    )
    total_generated = sum(
        row["schemes"][scheme_name]["metrics"]["generated_count"]
        for row in selected
    )
    total_accepted = sum(
        row["schemes"][scheme_name]["metrics"]["accepted_count"]
        for row in selected
    )
    return {
        "checkpoint_count": len(selected),
        "unweighted_coverage": sum(
            row["schemes"][scheme_name]["metrics"]["coverage"]
            for row in selected
        ) / len(selected),
        "checkpoint_weighted_coverage": sum(
            row["weight"]
            * row["schemes"][scheme_name]["metrics"]["coverage"]
            for row in selected
        ) / weight_sum,
        "access_weighted_coverage": (
            total_covered / total_demands if total_demands else 0.0
        ),
        "request_per_demand": (
            total_generated / total_demands if total_demands else 0.0
        ),
        "checkpoint_weighted_request_per_demand": sum(
            row["weight"]
            * metric_ratio(row["schemes"][scheme_name], "generated_count")
            for row in selected
        ) / weight_sum,
        "accepted_request_per_demand": (
            total_accepted / total_demands if total_demands else 0.0
        ),
        "checkpoint_weighted_accepted_request_per_demand": sum(
            row["weight"]
            * metric_ratio(row["schemes"][scheme_name], "accepted_count")
            for row in selected
        ) / weight_sum,
    }


def aggregate_resource(
    rows: List[Dict],
    scheme_name: str,
    resource_name: str,
) -> Optional[Dict]:
    samples: List[Tuple[Dict, Dict]] = []
    for row in rows:
        if not row["validated"] or scheme_name not in row["schemes"]:
            continue
        resource = next(
            (
                item for item in row["schemes"][scheme_name].get("resources", [])
                if item["name"] == resource_name
            ),
            None,
        )
        if resource is not None:
            samples.append((row, resource["metrics"]))
    if not samples:
        return None
    weight_sum = sum(row["weight"] for row, _ in samples)
    primary_enqueued = sum(
        metrics["enqueued_by_role"].get("primary", 0)
        for _, metrics in samples
    )
    extra_enqueued = sum(
        metrics["enqueued_by_role"].get("extra", 0)
        for _, metrics in samples
    )
    total_demands = sum(metrics["demand_count"] for _, metrics in samples)
    total_sent = sum(
        sum(metrics["sent_by_role"].values()) for _, metrics in samples
    )
    primary_sent = sum(
        metrics["sent_by_role"].get("primary", 0)
        for _, metrics in samples
    )
    extra_sent = sum(
        metrics["sent_by_role"].get("extra", 0)
        for _, metrics in samples
    )
    primary_delay = sum(
        metrics["queue_delay_cycles_sum"].get("primary", 0)
        for _, metrics in samples
    )
    extra_delay = sum(
        metrics["queue_delay_cycles_sum"].get("extra", 0)
        for _, metrics in samples
    )
    duplicate_role_available = all(
        "duplicate_sent_by_role" in metrics for _, metrics in samples
    )
    primary_duplicates = sum(
        metrics.get("duplicate_sent_by_role", {}).get("primary", 0)
        for _, metrics in samples
    )
    extra_duplicates = sum(
        metrics.get("duplicate_sent_by_role", {}).get("extra", 0)
        for _, metrics in samples
    )
    primary_filtered = sum(
        metrics.get("recent_filtered_by_role", {}).get("primary", 0)
        for _, metrics in samples
    )
    extra_filtered = sum(
        metrics.get("recent_filtered_by_role", {}).get("extra", 0)
        for _, metrics in samples
    )
    primary_filter_candidates = primary_enqueued + primary_filtered
    extra_filter_candidates = extra_enqueued + extra_filtered
    lead_64_weighted = sum(
        row["weight"]
        * (
            metrics["covered_with_min_lead_cycles"].get(
                "64",
                metrics["covered_with_min_lead_cycles"].get(64, 0),
            )
            / metrics["demand_count"]
            if metrics["demand_count"] else 0.0
        )
        for row, metrics in samples
    ) / weight_sum
    return {
        "checkpoint_count": len(samples),
        "checkpoint_weighted_coverage": sum(
            row["weight"] * metrics["coverage"]
            for row, metrics in samples
        ) / weight_sum,
        "checkpoint_weighted_lead_64_coverage": lead_64_weighted,
        "checkpoint_weighted_sent_per_demand": sum(
            row["weight"]
            * (
                sum(metrics["sent_by_role"].values())
                / metrics["demand_count"]
                if metrics["demand_count"] else 0.0
            )
            for row, metrics in samples
        ) / weight_sum,
        "sent_per_demand": total_sent / total_demands if total_demands else 0.0,
        "primary_drop_rate": (
            sum(
                metrics["dropped_by_role"].get("primary", 0)
                for _, metrics in samples
            ) / primary_enqueued
            if primary_enqueued else 0.0
        ),
        "extra_drop_rate": (
            sum(
                metrics["dropped_by_role"].get("extra", 0)
                for _, metrics in samples
            ) / extra_enqueued
            if extra_enqueued else 0.0
        ),
        "primary_queue_delay_mean": (
            primary_delay / primary_sent if primary_sent else 0.0
        ),
        "extra_queue_delay_mean": (
            extra_delay / extra_sent if extra_sent else 0.0
        ),
        "fifo_late_rate": (
            sum(metrics["fifo_late_demand_count"] for _, metrics in samples)
            / total_demands
            if total_demands else 0.0
        ),
        "duplicate_sent_rate": (
            sum(metrics["duplicate_sent_count"] for _, metrics in samples)
            / total_sent
            if total_sent else 0.0
        ),
        "primary_duplicate_sent_rate": (
            primary_duplicates / primary_sent
            if duplicate_role_available and primary_sent else None
        ),
        "extra_duplicate_sent_rate": (
            extra_duplicates / extra_sent
            if duplicate_role_available and extra_sent else None
        ),
        "recent_filter_rate": (
            (primary_filtered + extra_filtered)
            / (primary_filter_candidates + extra_filter_candidates)
            if primary_filter_candidates + extra_filter_candidates else 0.0
        ),
        "primary_recent_filter_rate": (
            primary_filtered / primary_filter_candidates
            if primary_filter_candidates else 0.0
        ),
        "extra_recent_filter_rate": (
            extra_filtered / extra_filter_candidates
            if extra_filter_candidates else 0.0
        ),
    }


def aggregate_top2_delta(
    rows: List[Dict],
    top1_name: str,
    top2_name: str,
) -> Optional[float]:
    selected = [
        row for row in rows
        if row["validated"]
        and top1_name in row["schemes"]
        and top2_name in row["schemes"]
    ]
    if not selected:
        return None
    weight_sum = sum(row["weight"] for row in selected)
    return sum(
        row["weight"]
        * (
            row["schemes"][top2_name]["metrics"]["coverage"]
            - row["schemes"][top1_name]["metrics"]["coverage"]
        )
        for row in selected
    ) / weight_sum


def percent(value: Optional[float]) -> str:
    return "n/a" if value is None else f"{value:.2%}"


def write_markdown(path: pathlib.Path, result: Dict) -> None:
    rows = result["checkpoints"]
    lines = [
        "# TS-BOP Degree Study Summary",
        "",
        f"- Checkpoints present: `{len(rows)}`",
        f"- Fully validated checkpoints: `{sum(row['validated'] for row in rows)}`",
        "- Weighted values use the selected checkpoint weights and are not a full-suite performance estimate.",
        "",
        (
            "| checkpoint | valid | actual | dual | d1 | d2 raw t5 | "
            "d2 greedy t2 | d2 greedy t5 | d3 raw t5 | d3 greedy t2 |"
        ),
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in rows:
        values = row["coverage"]
        lines.append(
            f"| {row['name']} | {row['validated']} | "
            f"{percent(row['actual_coverage'])} | {percent(values.get('dual'))} | "
            f"{percent(values.get('d1'))} | {percent(values.get('d2_raw_t5'))} | "
            f"{percent(values.get('d2_greedy_t2'))} | "
            f"{percent(values.get('d2_greedy_t5'))} | "
            f"{percent(values.get('d3_raw_t5'))} | "
            f"{percent(values.get('d3_greedy_t2'))} |"
        )

    lines.extend([
        "",
        "## Validated Aggregate",
        "",
        (
            "| scheme | n | unweighted coverage | checkpoint-weighted "
            "coverage | access-weighted coverage | generated/demand | "
            "page-accepted/demand | weighted gain vs d1 | dual-gap recovery | "
            "access gain / extra request |"
        ),
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    d1_weighted = result["aggregates"].get("d1", {}).get(
        "checkpoint_weighted_coverage"
    )
    for label in SCHEMES:
        aggregate = result["aggregates"].get(label)
        if aggregate is None:
            continue
        gain = (
            aggregate["checkpoint_weighted_coverage"] - d1_weighted
            if d1_weighted is not None else None
        )
        lines.append(
            f"| {label} | {aggregate['checkpoint_count']} | "
            f"{percent(aggregate['unweighted_coverage'])} | "
            f"{percent(aggregate['checkpoint_weighted_coverage'])} | "
            f"{percent(aggregate['access_weighted_coverage'])} | "
            f"{aggregate['request_per_demand']:.3f} | "
            f"{aggregate['accepted_request_per_demand']:.3f} | "
            f"{percent(gain)} | "
            f"{percent(aggregate['dual_gap_recovery'])} | "
            f"{percent(aggregate['access_gain_per_extra_request'])} |"
        )

    lines.extend([
        "",
        "## Current Dual Runtime Traffic",
        "",
        "These rates come from gem5 runtime counters for the original dual BOP.",
        "",
        (
            "| n | generated/demand (checkpoint-weighted) | "
            "generated/demand (access-weighted) | "
            "issued/demand (checkpoint-weighted) | "
            "issued/demand (access-weighted) | "
            "PFQ-full drop/demand (access-weighted) |"
        ),
        "| ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    traffic = result["actual_bop_traffic"]
    if traffic is not None:
        lines.append(
            f"| {traffic['checkpoint_count']} | "
            f"{traffic['checkpoint_weighted_generated_per_demand']:.3f} | "
            f"{traffic['access_weighted_generated_per_demand']:.3f} | "
            f"{traffic['checkpoint_weighted_issued_per_demand']:.3f} | "
            f"{traffic['access_weighted_issued_per_demand']:.3f} | "
            f"{traffic['access_weighted_removed_full_per_demand']:.3f} |"
        )

    lines.extend([
        "",
        "## FIFO Constraint",
        "",
        (
            "| resource | n | sent coverage | lead >=64 cycles | sent/demand "
            "(checkpoint-weighted) | sent/demand (access-weighted) | "
            "recent filtered | duplicate total | duplicate primary | "
            "duplicate extra | primary drop | extra drop | "
            "primary queue cycles | extra queue cycles | FIFO late |"
        ),
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    for name, aggregate in result["resources"].items():
        if aggregate is None:
            continue
        lines.append(
            f"| {name} | {aggregate['checkpoint_count']} | "
            f"{percent(aggregate['checkpoint_weighted_coverage'])} | "
            f"{percent(aggregate['checkpoint_weighted_lead_64_coverage'])} | "
            f"{aggregate['checkpoint_weighted_sent_per_demand']:.3f} | "
            f"{aggregate['sent_per_demand']:.3f} | "
            f"{percent(aggregate['recent_filter_rate'])} | "
            f"{percent(aggregate['duplicate_sent_rate'])} | "
            f"{percent(aggregate['primary_duplicate_sent_rate'])} | "
            f"{percent(aggregate['extra_duplicate_sent_rate'])} | "
            f"{percent(aggregate['primary_drop_rate'])} | "
            f"{percent(aggregate['extra_drop_rate'])} | "
            f"{aggregate['primary_queue_delay_mean']:.2f} | "
            f"{aggregate['extra_queue_delay_mean']:.2f} | "
            f"{percent(aggregate['fifo_late_rate'])} |"
        )

    lines.extend([
        "",
        "## Teacher Top-2",
        "",
        "| scheme | checkpoint-weighted coverage delta vs top-1 |",
        "| --- | ---: |",
    ])
    for label, delta in result["top2_deltas"].items():
        lines.append(f"| {label} | {percent(delta)} |")
    path.write_text("\n".join(lines) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize TS-BOP replay results")
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("util/ts_bop_degree/checkpoints.json"),
    )
    parser.add_argument("--out-json", required=True, type=pathlib.Path)
    parser.add_argument("--out-md", type=pathlib.Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    rows: List[Dict] = []
    for entry in manifest:
        result_dir = pathlib.Path(entry["result_dir"])
        shortlist = load_optional(result_dir / "shortlist_corrected.json")
        if shortlist is None:
            continue
        params = load_optional(result_dir / "params_top1.json")
        threshold_fine = load_optional(result_dir / "threshold_fine.json")
        top2 = load_optional(result_dir / "teacher_top2.json")
        resources_t2 = load_optional(result_dir / "resources_t2.json")
        resources_t5 = load_optional(result_dir / "resources_t5.json")
        resources_dual = load_optional(result_dir / "resources_dual.json")
        resources_filter = load_optional(result_dir / "resources_filter.json")
        resources_filter_t3 = load_optional(
            result_dir / "resources_filter_t3.json"
        )
        schemes = scheme_map(
            (
                shortlist,
                params,
                threshold_fine,
                top2,
                resources_t2,
                resources_t5,
                resources_dual,
                resources_filter,
                resources_filter_t3,
            )
        )
        distribution_mismatches = replay_distribution_mismatches(
            shortlist,
            result_dir / "stats.txt",
        )
        scalar_match = shortlist.get("stats_validation", {}).get(
            "scalar_all_match",
            shortlist.get("stats_validation", {}).get("all_match", False),
        )
        validated = bool(scalar_match and not distribution_mismatches)
        rows.append({
            "name": entry["name"],
            "workload": entry["workload"],
            "weight": entry["weight"],
            "result_dir": str(result_dir),
            "validated": validated,
            "scalar_match": scalar_match,
            "distribution_mismatches": distribution_mismatches,
            "actual_coverage": shortlist["actual"]["coverage"],
            "actual_bop_traffic": actual_bop_traffic(
                result_dir / "stats.txt",
                shortlist["actual"]["demand_count"],
            ),
            "coverage": {
                label: coverage(schemes, scheme_name)
                for label, scheme_name in SCHEMES.items()
            },
            "schemes": schemes,
        })

    aggregates = {
        label: aggregate_scheme(rows, scheme_name)
        for label, scheme_name in SCHEMES.items()
    }
    d1_aggregate = aggregates.get("d1")
    dual_aggregate = aggregates.get("dual")
    if d1_aggregate is not None:
        dual_gap = (
            dual_aggregate["checkpoint_weighted_coverage"]
            - d1_aggregate["checkpoint_weighted_coverage"]
            if dual_aggregate is not None else 0.0
        )
        for aggregate in aggregates.values():
            if aggregate is None:
                continue
            extra_requests = (
                aggregate["request_per_demand"]
                - d1_aggregate["request_per_demand"]
            )
            aggregate["access_gain_per_extra_request"] = (
                (
                    aggregate["access_weighted_coverage"]
                    - d1_aggregate["access_weighted_coverage"]
                ) / extra_requests
                if extra_requests > 0 else None
            )
            aggregate["dual_gap_recovery"] = (
                (
                    aggregate["checkpoint_weighted_coverage"]
                    - d1_aggregate["checkpoint_weighted_coverage"]
                ) / dual_gap
                if dual_gap > 0 else None
            )
    resources = {
        label: aggregate_resource(rows, scheme_name, resource_name)
        for label, (scheme_name, resource_name) in RESOURCE_TARGETS.items()
    }
    top2_deltas = {
        label: aggregate_top2_delta(rows, top1, top2)
        for label, (top1, top2) in TOP2_PAIRS.items()
    }
    public_rows = []
    for row in rows:
        public_row = {
            key: value for key, value in row.items() if key != "schemes"
        }
        public_row["scheme_metrics"] = {
            label: {
                "coverage": row["schemes"][scheme_name]["metrics"]["coverage"],
                "request_per_demand": metric_ratio(
                    row["schemes"][scheme_name],
                    "generated_count",
                ),
                "accepted_request_per_demand": metric_ratio(
                    row["schemes"][scheme_name],
                    "accepted_count",
                ),
            }
            for label, scheme_name in SCHEMES.items()
            if scheme_name in row["schemes"]
        }
        public_rows.append(public_row)
    result = {
        "manifest": str(args.manifest),
        "checkpoints": public_rows,
        "actual_bop_traffic": aggregate_actual_bop_traffic(rows),
        "aggregates": aggregates,
        "resources": resources,
        "top2_deltas": top2_deltas,
    }
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if args.out_md:
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        write_markdown(args.out_md, result)


if __name__ == "__main__":
    main()
