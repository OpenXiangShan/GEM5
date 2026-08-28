#!/usr/bin/env python3
"""Measure static set pressure for candidate IdealConstantLVP tables.

The input profile is an oracle table: entries are never evicted. This tool
does not claim to model temporal replacement. It answers the narrower,
necessary question of how the final per-slice static PC population maps into
candidate set-associative geometries.
"""

import argparse
import csv
import json
import math
import sys
from collections import Counter
from pathlib import Path

from ideal_constant_lvp_profile import (
    PROFILE_FILE,
    SCOPES,
    discover_profiles,
    percentile,
    read_profile,
    slice_id_for,
    write_csv,
)


ENTRY_CLASSES = ("all", "ever-saturated", "saturated-at-end")


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
        help="Profile phase to evaluate (default: lifetime).",
    )
    parser.add_argument(
        "--table",
        action="append",
        required=True,
        metavar="NAME:ENTRIES:WAYS:ENTRY_CLASS",
        help=(
            "Candidate table. ENTRY_CLASS is one of all, ever-saturated, or "
            "saturated-at-end. Repeat --table for a sweep."
        ),
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        required=True,
        help="Directory for per-slice and summary CSV/JSON reports.",
    )
    return parser.parse_args()


def is_power_of_two(value):
    return value > 0 and value & (value - 1) == 0


def parse_table(specification):
    fields = specification.split(":")
    if len(fields) != 4:
        raise ValueError(
            "table must be NAME:ENTRIES:WAYS:ENTRY_CLASS, got "
            f"{specification!r}"
        )

    name, entries_text, ways_text, entry_class = fields
    try:
        entries = int(entries_text)
        ways = int(ways_text)
    except ValueError as error:
        raise ValueError(f"table has non-integer geometry: {specification!r}") from error
    if not name:
        raise ValueError("table name must not be empty")
    if entry_class not in ENTRY_CLASSES:
        raise ValueError(
            f"unknown entry class {entry_class!r}; choose from {ENTRY_CLASSES}"
        )
    if entries <= 0 or ways <= 0 or entries % ways:
        raise ValueError(
            "entries and ways must be positive and entries must divide evenly "
            f"by ways: {specification!r}"
        )
    sets = entries // ways
    if not is_power_of_two(sets):
        raise ValueError(
            f"number of sets must be a power of two, got {sets} in {specification!r}"
        )
    return {
        "name": name,
        "entries": entries,
        "ways": ways,
        "sets": sets,
        "entry_class": entry_class,
    }


def index_for_pc(pc, sets):
    """Index 16-bit compressed and 32-bit RISC-V instruction PCs uniformly."""
    folded_pc = pc >> 1
    folded_pc ^= folded_pc >> 11
    folded_pc ^= folded_pc >> 22
    folded_pc ^= folded_pc >> 33
    return folded_pc & (sets - 1)


def select_rows(rows, entry_class):
    if entry_class == "all":
        return rows
    field = "ever_saturated" if entry_class == "ever-saturated" else "saturated_at_end"
    return [row for row in rows if row[field]]


def capacity_row(profile_path, scope, table):
    _, rows = read_profile(profile_path, scope)
    selected = select_rows(rows, table["entry_class"])
    set_occupancy = Counter()
    for row in selected:
        index = index_for_pc(row["pc"], table["sets"])
        set_occupancy[(row["tid"], index)] += 1

    occupancies = list(set_occupancy.values())
    overflow_entries = sum(
        max(0, occupancy - table["ways"]) for occupancy in occupancies
    )
    overfull_sets = sum(occupancy > table["ways"] for occupancy in occupancies)
    return {
        "table": table["name"],
        "slice": slice_id_for(profile_path),
        "profile": str(profile_path),
        "scope": scope,
        "entry_class": table["entry_class"],
        "entries": table["entries"],
        "ways": table["ways"],
        "sets": table["sets"],
        "selected_pcs": len(selected),
        "max_set_occupancy": max(occupancies, default=0),
        "overfull_sets": overfull_sets,
        "static_overflow_entries": overflow_entries,
    }


def summary_for_table(table, rows):
    selected_pcs = [row["selected_pcs"] for row in rows]
    max_set_occupancy = [row["max_set_occupancy"] for row in rows]
    overflow_entries = [row["static_overflow_entries"] for row in rows]
    overfull_sets = [row["overfull_sets"] for row in rows]
    return {
        "table": table["name"],
        "entry_class": table["entry_class"],
        "entries": table["entries"],
        "ways": table["ways"],
        "sets": table["sets"],
        "slices": len(rows),
        "selected_pcs_max": max(selected_pcs, default=0),
        "selected_pcs_p95": percentile(selected_pcs, 95),
        "max_set_occupancy_max": max(max_set_occupancy, default=0),
        "max_set_occupancy_p95": percentile(max_set_occupancy, 95),
        "static_overflow_entries_max": max(overflow_entries, default=0),
        "static_overflow_entries_p95": percentile(overflow_entries, 95),
        "slices_with_static_overflow": sum(value > 0 for value in overflow_entries),
        "overfull_sets_max": max(overfull_sets, default=0),
    }


def main():
    args = parse_args()
    try:
        profiles = discover_profiles(args.inputs)
        tables = [parse_table(specification) for specification in args.table]
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if not profiles:
        print(f"error: no {PROFILE_FILE} files found", file=sys.stderr)
        return 2
    if len({table["name"] for table in tables}) != len(tables):
        print("error: table names must be unique", file=sys.stderr)
        return 2

    rows = []
    for table in tables:
        rows.extend(capacity_row(profile, args.scope, table) for profile in profiles)
    rows.sort(key=lambda row: (row["table"], row["slice"]))

    summaries = [
        summary_for_table(
            table, [row for row in rows if row["table"] == table["name"]]
        )
        for table in tables
    ]
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(
        args.out_dir / "per_slice_set_pressure.csv",
        [
            "table",
            "slice",
            "profile",
            "scope",
            "entry_class",
            "entries",
            "ways",
            "sets",
            "selected_pcs",
            "max_set_occupancy",
            "overfull_sets",
            "static_overflow_entries",
        ],
        rows,
    )
    write_csv(
        args.out_dir / "table_set_pressure_summary.csv",
        [
            "table",
            "entry_class",
            "entries",
            "ways",
            "sets",
            "slices",
            "selected_pcs_max",
            "selected_pcs_p95",
            "max_set_occupancy_max",
            "max_set_occupancy_p95",
            "static_overflow_entries_max",
            "static_overflow_entries_p95",
            "slices_with_static_overflow",
            "overfull_sets_max",
        ],
        summaries,
    )
    with (args.out_dir / "table_set_pressure_summary.json").open("w") as output:
        json.dump(
            {
                "scope": args.scope,
                "profile_files": len(profiles),
                "index": "(pc >> 1) xor-folded at shifts 11, 22, and 33",
                "warning": (
                    "Static final-PC set pressure is not a temporal replacement "
                    "simulation and must not be used as a coverage estimate."
                ),
                "tables": summaries,
            },
            output,
            indent=2,
            sort_keys=True,
        )
        output.write("\n")
    print(json.dumps(summaries, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
