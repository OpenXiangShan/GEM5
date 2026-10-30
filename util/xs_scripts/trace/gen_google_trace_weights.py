#!/usr/bin/env python3
"""Generate fixed RV64 instruction weights from Google SimPoint sample logs."""

import argparse
import json
import math
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace_root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    workloads = {}
    for line in (args.trace_root / "google_traces.lst").read_text().splitlines():
        name, relative, _, _, _, sample = line.split()
        match = re.search(r"_([0-9.eE+-]+)_\.trace$", relative)
        if not match:
            raise ValueError(f"missing SimPoint weight: {relative}")
        weight = float(match[1])
        log = (args.trace_root / (relative + ".log")).read_text()
        interval = re.search(r"Sampling Interval from (\d+) to (\d+)", log)
        if not interval:
            raise ValueError(f"missing sampling counts: {relative}")
        source, rv64 = map(int, interval.groups())
        if (
            not math.isfinite(weight) or weight <= 0
            or source <= 0 or rv64 <= 0 or rv64 != int(sample)
        ):
            raise ValueError(f"invalid sampling counts/weight: {relative}")
        points = workloads.setdefault(Path(relative).parts[0], {})
        if name in points:
            raise ValueError(f"duplicate trace: {name}")
        points[name] = {
            "source_weight": weight,
            "source_sample_insts": source,
            "rv64_sample_insts": rv64,
            "rv64_weight": weight * rv64 / source,
        }
    if not workloads:
        raise ValueError("empty Google trace list")
    for workload, points in workloads.items():
        if not math.isclose(
            sum(p["source_weight"] for p in points.values()), 1, abs_tol=1e-5
        ):
            raise ValueError(f"incomplete source SimPoint weights: {workload}")
        total = sum(p["rv64_weight"] for p in points.values())
        for point in points.values():
            point["rv64_weight"] /= total
    args.output.write_text(json.dumps({
        "formula": "normalize(source_weight * rv64_sample_insts / "
        "source_sample_insts) within each workload",
        "workloads": workloads,
    }, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
