#!/usr/bin/env python3

import argparse
import csv
import os
import re
import sys


TOTAL_IPC_RE = re.compile(r"^system\.cpu\.totalIpc\s+(\S+)\s+")
BEGIN_STATS = "---------- Begin Simulation Statistics ----------"


def workload_point_from_relpath(rel_dir):
    parts = rel_dir.split(os.sep)
    first = parts[0]
    first_tokens = first.split("_")

    if len(parts) >= 2 and parts[1].isdigit() and len(parts[1]) > 1:
        return first, parts[1]
    if len(first_tokens) >= 4 and not first_tokens[2].isdigit() and first_tokens[3].isdigit():
        return "_".join(first_tokens[:3]), first_tokens[3]
    if len(first_tokens) >= 3 and first_tokens[2].isdigit():
        return "_".join(first_tokens[:2]), first_tokens[2]
    if len(first_tokens) == 2 and first_tokens[1].isdigit():
        return first_tokens[0], first_tokens[1]
    return first, "0"


def extract_last_total_ipc(stats_path):
    total_ipc = None

    with open(stats_path, "r", encoding="utf-8", errors="replace") as stats_file:
        for line in stats_file:
            if line.startswith(BEGIN_STATS):
                total_ipc = None
            match = TOTAL_IPC_RE.match(line)
            if match:
                total_ipc = match.group(1)

    return total_ipc


def main():
    parser = argparse.ArgumentParser(
        description="Extract gem5 SMT total IPC into gem5_data_proc score CSV format."
    )
    parser.add_argument("stats_root", help="Root directory containing workload stats.txt files.")
    parser.add_argument("output_csv", help="CSV file consumed by compute_weighted.py.")
    args = parser.parse_args()

    rows = []
    for dirpath, _, filenames in os.walk(args.stats_root):
        if "stats.txt" not in filenames:
            continue
        stats_path = os.path.join(dirpath, "stats.txt")
        rel = os.path.relpath(dirpath, args.stats_root)
        workload, point = workload_point_from_relpath(rel)
        try:
            ipc = extract_last_total_ipc(stats_path)
        except OSError as exc:
            print(f"warning: failed to read {stats_path}: {exc}", file=sys.stderr)
            continue
        if ipc is None:
            print(f"warning: missing system.cpu.totalIpc in {stats_path}", file=sys.stderr)
            continue
        rows.append({
            "": f"{workload}_{point}",
            "bmk": workload.split("_")[0],
            "ipc": ipc,
            "point": point,
            "workload": workload,
        })

    rows.sort(key=lambda row: row[""])
    if not rows:
        print(f"warning: no usable stats.txt files found under {args.stats_root}", file=sys.stderr)

    output_dir = os.path.dirname(args.output_csv)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
    with open(args.output_csv, "w", newline="", encoding="utf-8") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=["", "bmk", "ipc", "point", "workload"])
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
