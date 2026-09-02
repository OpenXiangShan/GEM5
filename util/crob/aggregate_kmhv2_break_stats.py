#!/usr/bin/env python3

"""Aggregate kmhv2 CROB interruption counters from gem5 CI slices."""

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path


CLASS_NAMES = (
    "simpleIntegerAlu",
    "simpleFloatingAlu",
    "simpleOther",
    "load",
    "store",
    "branch",
    "jump",
    "otherComplex",
)
SIMPLE_CLASSES = CLASS_NAMES[:3]
COMPLEX_CLASSES = CLASS_NAMES[3:]
METRIC_NAMES = (
    "simple_fraction",
    "breaking_complex_fraction",
    "break_blocks_per_1k_inst",
    "break_lost_fraction",
    "actual_rob_density",
    "no_break_rob_density",
)


def safe_ratio(numerator, denominator):
    return numerator / denominator if denominator else 0.0


def read_stats(path):
    stats = {}
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            fields = line.split()
            if len(fields) < 2 or fields[0].startswith("-"):
                continue
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                continue
    return stats


def stat_value(stats, name, default=None):
    suffix = f".commit.{name}"
    matches = [value for key, value in stats.items() if key.endswith(suffix)]
    if matches:
        return sum(matches)
    if default is not None:
        return default
    raise ValueError(f"CROB kmhv2 counters missing required entry: {name}")


def split_slice_name(slice_name):
    try:
        workload, point = slice_name.rsplit("_", 1)
    except ValueError as exc:
        raise ValueError(
            f"slice directory must end in _<point>: {slice_name}"
        ) from exc
    return workload, point


def read_weights(path):
    with Path(path).open(encoding="utf-8") as handle:
        raw = json.load(handle)
    return {
        (workload, str(point)): float(weight)
        for workload, entry in raw.items()
        for point, weight in entry["points"].items()
    }


def extract_slice(stats_path, weight_map):
    slice_name = stats_path.parent.parent.name
    workload, point = split_slice_name(slice_name)
    if (workload, point) not in weight_map:
        raise ValueError(f"missing simpoint weight for slice {slice_name}")
    stats = read_stats(stats_path)

    analyzed_bundles = stat_value(stats, "crobKmhv2AnalyzedBundles")
    class_counts = {
        name: stat_value(stats, f"crobKmhv2InstClass::{name}", 0.0)
        for name in CLASS_NAMES
    }
    breaking_counts = {
        name: stat_value(stats, f"crobKmhv2BreakingInstClass::{name}", 0.0)
        for name in COMPLEX_CLASSES
    }
    simple_runs = {}
    run_marker = ".commit.crobKmhv2SimpleRunLength::"
    ignored_bins = {
        "samples", "mean", "stdev", "underflows", "overflows",
        "min_value", "max_value", "total",
    }
    for key, value in stats.items():
        if run_marker not in key:
            continue
        bin_name = key.rsplit("::", 1)[1]
        if bin_name not in ignored_bins:
            simple_runs[bin_name] = simple_runs.get(bin_name, 0.0) + value

    analyzed_insts = sum(class_counts.values())
    simple_insts = sum(class_counts[name] for name in SIMPLE_CLASSES)
    complex_insts = sum(class_counts[name] for name in COMPLEX_CLASSES)
    breaking_insts = sum(breaking_counts.values())
    physical_entries = stat_value(stats, "crobKmhv2PhysicalEntries")
    no_break_entries = stat_value(stats, "crobKmhv2NoBreakPhysicalEntries")
    lost_entries = stat_value(stats, "crobKmhv2BreakLostEntries")
    break_blocks = stat_value(stats, "crobKmhv2BreakBlocks")

    row = {
        "slice": slice_name,
        "workload": workload,
        "point": point,
        "weight": weight_map[(workload, point)],
        "normalized_weight": 0.0,
        "analyzed_bundles": analyzed_bundles,
        "analyzed_insts": analyzed_insts,
        "simple_insts": simple_insts,
        "complex_insts": complex_insts,
        "breaking_complex_insts": breaking_insts,
        "break_blocks": break_blocks,
        "physical_entries": physical_entries,
        "no_break_physical_entries": no_break_entries,
        "break_lost_entries": lost_entries,
        "simple_fraction": safe_ratio(simple_insts, analyzed_insts),
        "breaking_complex_fraction": safe_ratio(breaking_insts, complex_insts),
        "break_blocks_per_1k_inst": 1000.0 * safe_ratio(
            break_blocks, analyzed_insts
        ),
        "break_lost_fraction": safe_ratio(lost_entries, physical_entries),
        "actual_rob_density": safe_ratio(analyzed_insts, physical_entries),
        "no_break_rob_density": safe_ratio(analyzed_insts, no_break_entries),
        "stats_path": str(stats_path),
        "_breaking_counts": breaking_counts,
        "_simple_runs": simple_runs,
    }
    return row


def collect_results(stats_root, weights_path):
    stats_root = Path(stats_root)
    weight_map = read_weights(weights_path)
    stats_paths = sorted(stats_root.glob("*/m5out/stats.txt"))
    if not stats_paths:
        raise ValueError(f"no */m5out/stats.txt files found under {stats_root}")

    slices = [extract_slice(path, weight_map) for path in stats_paths]
    grouped = defaultdict(list)
    for row in slices:
        grouped[row["workload"]].append(row)

    workloads = []
    break_distribution = []
    run_distribution = []
    workload_break_fractions = {}
    workload_run_fractions = {}
    for workload, rows in sorted(grouped.items()):
        total_weight = sum(row["weight"] for row in rows)
        if total_weight <= 0:
            raise ValueError(f"no positive simpoint weights found for {workload}")
        for row in rows:
            row["normalized_weight"] = row["weight"] / total_weight

        workload_row = {
            "workload": workload,
            "slice_count": len(rows),
            "covered_weight": total_weight,
        }
        for metric in METRIC_NAMES:
            workload_row[metric] = sum(
                row["normalized_weight"] * row[metric] for row in rows
            )
        workloads.append(workload_row)

        weighted_break_fraction = defaultdict(float)
        weighted_run_fraction = defaultdict(float)
        for row in rows:
            break_total = sum(row["_breaking_counts"].values())
            run_total = sum(row["_simple_runs"].values())
            for name, count in row["_breaking_counts"].items():
                weighted_break_fraction[name] += row["normalized_weight"] * safe_ratio(
                    count, break_total
                )
            for length, count in row["_simple_runs"].items():
                weighted_run_fraction[length] += row["normalized_weight"] * safe_ratio(
                    count, run_total
                )

        break_norm = sum(weighted_break_fraction.values())
        workload_break_fractions[workload] = {
            name: safe_ratio(weighted_break_fraction[name], break_norm)
            for name in COMPLEX_CLASSES
        }
        for name in COMPLEX_CLASSES:
            break_distribution.append({
                "workload": workload,
                "instruction_class": name,
                "fraction": workload_break_fractions[workload][name],
            })
        run_norm = sum(weighted_run_fraction.values())
        workload_run_fractions[workload] = {
            length: safe_ratio(value, run_norm)
            for length, value in weighted_run_fraction.items()
        }
        for length in sorted(
            weighted_run_fraction,
            key=lambda value: int(value.split("-", 1)[0]),
        ):
            run_distribution.append({
                "workload": workload,
                "simple_run_length": length,
                "fraction": safe_ratio(weighted_run_fraction[length], run_norm),
            })

    workload_count = len(workloads)
    suite_row = {
        "workload": "__suite_mean__",
        "slice_count": sum(row["slice_count"] for row in workloads),
        "covered_weight": sum(row["covered_weight"] for row in workloads)
        / workload_count,
    }
    for metric in METRIC_NAMES:
        suite_row[metric] = sum(row[metric] for row in workloads) / workload_count
    workloads.append(suite_row)

    active_break_workloads = [
        fractions for fractions in workload_break_fractions.values()
        if sum(fractions.values()) > 0
    ]
    if active_break_workloads:
        for name in COMPLEX_CLASSES:
            break_distribution.append({
                "workload": "__suite_mean__",
                "instruction_class": name,
                "fraction": sum(row[name] for row in active_break_workloads)
                / len(active_break_workloads),
            })

    active_run_workloads = [
        fractions for fractions in workload_run_fractions.values()
        if sum(fractions.values()) > 0
    ]
    run_lengths = {
        length for fractions in active_run_workloads for length in fractions
    }
    for length in sorted(run_lengths, key=lambda value: int(value.split("-", 1)[0])):
        run_distribution.append({
            "workload": "__suite_mean__",
            "simple_run_length": length,
            "fraction": sum(row.get(length, 0.0) for row in active_run_workloads)
            / len(active_run_workloads),
        })

    public_slices = [
        {key: value for key, value in row.items() if not key.startswith("_")}
        for row in slices
    ]
    return public_slices, workloads, break_distribution, run_distribution


def write_csv(path, rows, fieldnames):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "stats_root",
        help="CI spec_all directory containing <workload>_<point>/m5out/stats.txt",
    )
    parser.add_argument("--weights", required=True, help="CI cluster JSON file")
    parser.add_argument("--output-dir", default="crob-kmhv2-analysis")
    args = parser.parse_args(argv)

    slices, workloads, break_distribution, run_distribution = collect_results(
        args.stats_root, args.weights
    )
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    write_csv(
        output_dir / "crob_kmhv2_slices.csv",
        slices,
        list(slices[0]),
    )
    write_csv(
        output_dir / "crob_kmhv2_workloads.csv",
        workloads,
        list(workloads[0]),
    )
    write_csv(
        output_dir / "crob_kmhv2_breaking_instruction_distribution.csv",
        break_distribution,
        ("workload", "instruction_class", "fraction"),
    )
    write_csv(
        output_dir / "crob_kmhv2_simple_run_distribution.csv",
        run_distribution,
        ("workload", "simple_run_length", "fraction"),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
