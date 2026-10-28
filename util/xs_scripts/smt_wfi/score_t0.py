#!/usr/bin/env python3
"""Score only T0; keep the raw SMT statistics and shared data processor intact."""

import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys

from validate import jobs, validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--cluster", type=Path, required=True)
    parser.add_argument("--data-proc", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    cluster = json.loads(args.cluster.read_text())
    rows = []
    for job in jobs(args.archive):
        report, stats = validate(job / "m5out")
        matches = []
        for workload in cluster:
            prefix = workload + "_"
            if job.name.startswith(prefix):
                point = job.name[len(prefix):].split("_", 1)[0]
                if point in cluster[workload]["points"]:
                    matches.append((workload, point))
        if len(matches) != 1:
            raise ValueError(f"ambiguous or missing weight key for {job.name}")
        workload, point = matches[0]
        insts = report["measurement_insts"][0]
        cycles = stats["system.cpu.numCycles"]
        rows.append({"slice": job.name, "bmk": workload.split("_")[0],
                     "workload": workload, "point": point,
                     "ipc": insts / cycles, "cpi": cycles / insts,
                     "cycles": cycles, "committedInsts": insts,
                     "tid1_committedInsts": report["measurement_insts"][1],
                     "total_ipc": sum(report["measurement_insts"]) / cycles})
    args.output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = args.output_dir / "t0-input.csv"
    with csv_path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    processor = args.data_proc / "simpoint_cpt/compute_weighted.py"
    print("Scoring mode: T0 committed instructions / shared CPU cycles", flush=True)
    subprocess.run(
        [sys.executable, str(processor.resolve()), "-r", str(csv_path.resolve()),
         "-j", str(args.cluster.resolve()),
         "--out-dir", str(args.output_dir.resolve()),
         "--score", str((args.output_dir / "t0-score.csv").resolve())], check=True,
    )


if __name__ == "__main__":
    main()
