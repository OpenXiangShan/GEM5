#!/usr/bin/env python3
"""Validate a SPEC measurement with T1 waiting after a retired WFI."""

import argparse
import json
from pathlib import Path


def read_stats(path):
    blocks = []
    current = None
    for line in path.read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            current = {}
        elif "End Simulation Statistics" in line:
            if current is not None:
                blocks.append(current)
                current = None
        elif current is not None:
            fields = line.split()
            if len(fields) >= 2:
                try:
                    current[fields[0]] = float(fields[1])
                except ValueError:
                    pass
    return blocks


def validate(output):
    report = json.loads((output / "smt-wfi-loop.json").read_text())
    if (report["experiment"], report["num_threads"]) != ("wfi-wait", 2):
        raise ValueError("expected a two-thread WFI wait experiment")
    phases = report["phases"]
    if [p["phase"] for p in phases] != ["wfi_ready", "measurement_start", "measurement_end"]:
        raise ValueError("incomplete measurement")
    blocks = read_stats(output / "stats.txt")
    if len(blocks) != 2:
        raise ValueError("expected warmup and measurement statistics")
    stats = blocks[-1]
    counts = report["measurement_insts"]
    for tid in range(2):
        if (tid == 0 and counts[tid] <= 0) or (tid == 1 and counts[tid] != 0):
            raise ValueError(f"unexpected progress on T{tid}")
        if counts[tid] != stats[f"system.cpu.committedInsts::{tid}"]:
            raise ValueError(f"T{tid} instruction counts disagree")
        if counts[tid] != phases[2]["insts"][tid] - phases[1]["insts"][tid]:
            raise ValueError(f"T{tid} phase counts disagree")
    if len({p["insts"][1] for p in phases}) != 1:
        raise ValueError("T1 committed after entering WFI")
    for phase in phases:
        if (phase["allocation_threads"], phase["runnable_threads"]) != (2, 1):
            raise ValueError("WFI resource identity changed")
        if phase["wfi_entries"] != 1 or phase["wfi_wakeups"] != 0:
            raise ValueError("unexpected WFI entry/wake during measurement")
        running, waiting = phase["rob_lsq"]
        if (not running["active"] or waiting["active"] or
                running["limits"] != report["rob_lsq_capacity"] or
                waiting["limits"] != [0, 0, 0] or
                waiting["used"] != [0, 0, 0] or
                waiting["free"] != [0, 0, 0]):
            raise ValueError("incorrect WFI ROB/LQ/SQ allocation")
    for key in ("system.cpu.committedOps::1",
                "system.cpu.branchPred.predictionsStartedByThread::1"):
        if stats[key] != 0:
            raise ValueError(f"T1 activity while waiting: {key}")
    cycles = stats["system.cpu.numCycles"]
    if cycles <= 0 or report["measurement_ticks"] <= 0:
        raise ValueError("empty measurement interval")
    if phases[2]["insts"][0] < report["stop_total_insts"]:
        raise ValueError("T0 stopped before the requested total instruction count")
    if sum(counts) != stats["system.cpu.committedInsts::total"]:
        raise ValueError("aggregate count disagrees")
    print(f"PASS {output}: T0={counts[0]}, T1={counts[1]}, "
          f"cycles={int(cycles)}, T0_IPC={counts[0] / cycles:.6f}, "
          f"total_IPC={sum(counts) / cycles:.6f}")
    return report, stats


def jobs(archive):
    found = sorted(p for p in archive.iterdir() if p.is_dir())
    if not found:
        raise ValueError("empty experiment archive")
    for job in found:
        if (job / "abort").exists() or not (job / "completed").exists():
            raise ValueError(f"failed or incomplete workload: {job}")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--output", type=Path)
    group.add_argument("--archive", type=Path)
    args = parser.parse_args()
    if args.output:
        validate(args.output)
    else:
        for job in jobs(args.archive):
            validate(job / "m5out")


if __name__ == "__main__":
    main()
