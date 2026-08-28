#!/usr/bin/env python3
"""Merge IdealConstantLVP per-PC profiles from a GEM5 performance archive."""

import argparse
import csv
import json
import math
import re
import sys
from collections import defaultdict
from pathlib import Path


PROFILE_FILE = "ideal_constant_lvp_profile.csv"
SCOPES = ("lifetime", "roi")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "inputs",
        nargs="+",
        type=Path,
        help="Archive roots, spec_all roots, slice roots, or profile CSV files.",
    )
    parser.add_argument(
        "--scope",
        choices=SCOPES,
        default="lifetime",
        help="Profile phase to aggregate (default: lifetime).",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        required=True,
        help="Directory for per-slice, per-benchmark, and global reports.",
    )
    parser.add_argument(
        "--benchmark-regex",
        default=r"^(?P<benchmark>[^_]+)_",
        help=(
            "Regex applied to each slice directory name. It must define a "
            "'benchmark' group (default extracts the prefix before '_')."
        ),
    )
    return parser.parse_args()


def discover_profiles(inputs):
    profiles = set()
    for path in inputs:
        if path.is_file():
            if path.name != PROFILE_FILE:
                raise ValueError(f"Expected {PROFILE_FILE}, got {path}")
            profiles.add(path.resolve())
            continue
        if not path.is_dir():
            raise ValueError(f"Input does not exist: {path}")
        profiles.update(candidate.resolve() for candidate in path.rglob(PROFILE_FILE))
    return sorted(profiles)


def slice_id_for(profile_path):
    if profile_path.parent.name == "m5out":
        return profile_path.parent.parent.name
    return profile_path.parent.name


def benchmark_for(slice_id, pattern):
    match = pattern.match(slice_id)
    if not match:
        return slice_id
    return match.group("benchmark")


def read_profile(profile_path, scope):
    metadata = {}
    data_lines = []
    with profile_path.open(newline="") as profile_file:
        for line in profile_file:
            if line.startswith("#"):
                key, separator, value = line[1:].strip().partition("=")
                if separator:
                    metadata[key] = value
                continue
            data_lines.append(line)

    rows = []
    for row in csv.DictReader(data_lines):
        if row["scope"] != scope:
            continue
        rows.append(
            {
                "tid": int(row["tid"]),
                "pc": int(row["pc"], 0),
                "updates": int(row["updates"]),
                "first_update": int(row["first_update"]),
                "last_update": int(row["last_update"]),
                "value_changes": int(row["value_changes"]),
                "saturation_transitions": int(row["saturation_transitions"]),
                "saturated_updates": int(row["saturated_updates"]),
                "first_saturation_update": int(row["first_saturation_update"]),
                "ever_saturated": int(row["ever_saturated"]),
                "saturated_at_end": int(row["saturated_at_end"]),
                "confidence": int(row["confidence"]),
            }
        )
    return metadata, rows


def percentile(values, percentage):
    if not values:
        return 0
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * percentage / 100) - 1)
    return ordered[index]


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main():
    args = parse_args()
    try:
        benchmark_pattern = re.compile(args.benchmark_regex)
        if "benchmark" not in benchmark_pattern.groupindex:
            raise ValueError("--benchmark-regex must define a 'benchmark' group")
        profiles = discover_profiles(args.inputs)
    except (OSError, ValueError, re.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if not profiles:
        print(f"error: no {PROFILE_FILE} files found", file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    per_slice = []
    per_pc = {}
    metadata_values = defaultdict(set)

    for profile_path in profiles:
        metadata, rows = read_profile(profile_path, args.scope)
        for key, value in metadata.items():
            metadata_values[key].add(value)

        slice_id = slice_id_for(profile_path)
        benchmark = benchmark_for(slice_id, benchmark_pattern)
        if not rows:
            print(
                f"warning: {profile_path} has no {args.scope} rows",
                file=sys.stderr,
            )

        distinct_pcs = len(rows)
        ever_saturated = sum(row["ever_saturated"] for row in rows)
        saturated_at_end = sum(row["saturated_at_end"] for row in rows)
        per_slice.append(
            {
                "benchmark": benchmark,
                "slice": slice_id,
                "profile": str(profile_path),
                "distinct_pcs": distinct_pcs,
                "ever_saturated_pcs": ever_saturated,
                "saturated_at_end_pcs": saturated_at_end,
                "updates": sum(row["updates"] for row in rows),
                "value_changes": sum(row["value_changes"] for row in rows),
                "saturation_transitions": sum(
                    row["saturation_transitions"] for row in rows
                ),
            }
        )

        for row in rows:
            key = (benchmark, row["tid"], row["pc"])
            aggregate = per_pc.setdefault(
                key,
                {
                    "benchmark": benchmark,
                    "tid": row["tid"],
                    "pc": f"0x{row['pc']:x}",
                    "slices_seen": 0,
                    "updates": 0,
                    "value_changes": 0,
                    "saturation_transitions": 0,
                    "saturated_updates": 0,
                    "ever_saturated": 0,
                    "saturated_at_end_slices": 0,
                },
            )
            aggregate["slices_seen"] += 1
            aggregate["updates"] += row["updates"]
            aggregate["value_changes"] += row["value_changes"]
            aggregate["saturation_transitions"] += row["saturation_transitions"]
            aggregate["saturated_updates"] += row["saturated_updates"]
            aggregate["ever_saturated"] |= row["ever_saturated"]
            aggregate["saturated_at_end_slices"] += row["saturated_at_end"]

    per_slice.sort(key=lambda row: (row["benchmark"], row["slice"]))
    per_pc_rows = sorted(
        per_pc.values(), key=lambda row: (row["benchmark"], row["tid"], row["pc"])
    )

    benchmark_rows = []
    for benchmark in sorted({row["benchmark"] for row in per_slice}):
        slice_rows = [row for row in per_slice if row["benchmark"] == benchmark]
        pc_rows = [row for row in per_pc_rows if row["benchmark"] == benchmark]
        benchmark_rows.append(
            {
                "benchmark": benchmark,
                "slices": len(slice_rows),
                "sum_slice_distinct_pcs": sum(
                    row["distinct_pcs"] for row in slice_rows
                ),
                "unique_pcs_across_slices": len(pc_rows),
                "pcs_ever_saturated": sum(
                    row["ever_saturated"] for row in pc_rows
                ),
                "pcs_saturated_at_end_in_any_slice": sum(
                    row["saturated_at_end_slices"] > 0 for row in pc_rows
                ),
                "max_slice_distinct_pcs": max(
                    row["distinct_pcs"] for row in slice_rows
                ),
                "p95_slice_distinct_pcs": percentile(
                    [row["distinct_pcs"] for row in slice_rows], 95
                ),
                "max_slice_ever_saturated_pcs": max(
                    row["ever_saturated_pcs"] for row in slice_rows
                ),
                "p95_slice_ever_saturated_pcs": percentile(
                    [row["ever_saturated_pcs"] for row in slice_rows], 95
                ),
            }
        )

    all_distinct = [row["distinct_pcs"] for row in per_slice]
    all_saturated = [row["ever_saturated_pcs"] for row in per_slice]
    summary = {
        "profile_scope": args.scope,
        "profile_files": len(profiles),
        "slices": len(per_slice),
        "slice_distinct_pcs": {
            "sum": sum(all_distinct),
            "max": max(all_distinct),
            "p50": percentile(all_distinct, 50),
            "p90": percentile(all_distinct, 90),
            "p95": percentile(all_distinct, 95),
            "p99": percentile(all_distinct, 99),
        },
        "slice_ever_saturated_pcs": {
            "sum": sum(all_saturated),
            "max": max(all_saturated),
            "p50": percentile(all_saturated, 50),
            "p90": percentile(all_saturated, 90),
            "p95": percentile(all_saturated, 95),
            "p99": percentile(all_saturated, 99),
        },
        "corpus_unique_pc_items": len(per_pc_rows),
        "corpus_pc_items_ever_saturated": sum(
            row["ever_saturated"] for row in per_pc_rows
        ),
        "corpus_pc_items_saturated_at_end_in_any_slice": sum(
            row["saturated_at_end_slices"] > 0 for row in per_pc_rows
        ),
        "metadata": {
            key: sorted(values) for key, values in sorted(metadata_values.items())
        },
        "key_definition": "benchmark, tid, PC",
        "benchmark_definition": args.benchmark_regex,
    }

    write_csv(
        args.out_dir / "per_slice.csv",
        [
            "benchmark",
            "slice",
            "profile",
            "distinct_pcs",
            "ever_saturated_pcs",
            "saturated_at_end_pcs",
            "updates",
            "value_changes",
            "saturation_transitions",
        ],
        per_slice,
    )
    write_csv(
        args.out_dir / "per_benchmark.csv",
        [
            "benchmark",
            "slices",
            "sum_slice_distinct_pcs",
            "unique_pcs_across_slices",
            "pcs_ever_saturated",
            "pcs_saturated_at_end_in_any_slice",
            "max_slice_distinct_pcs",
            "p95_slice_distinct_pcs",
            "max_slice_ever_saturated_pcs",
            "p95_slice_ever_saturated_pcs",
        ],
        benchmark_rows,
    )
    write_csv(
        args.out_dir / "per_pc.csv",
        [
            "benchmark",
            "tid",
            "pc",
            "slices_seen",
            "updates",
            "value_changes",
            "saturation_transitions",
            "saturated_updates",
            "ever_saturated",
            "saturated_at_end_slices",
        ],
        per_pc_rows,
    )
    with (args.out_dir / "summary.json").open("w") as output_file:
        json.dump(summary, output_file, indent=2, sort_keys=True)
        output_file.write("\n")

    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
