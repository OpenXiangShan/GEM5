# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Summarize completed LRU/SDBP pairs; never merge warmup and measured stats."""

import argparse
import csv
import json
import math
from pathlib import Path
import re


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
            if len(fields) > 1:
                try:
                    current[fields[0]] = float(fields[1])
                except ValueError:
                    pass
    if not blocks:
        raise ValueError(f"No complete statistics block in {path}")
    return blocks[-1], len(blocks)


def metrics(directory, manifest):
    status = json.loads((directory / "status.json").read_text())
    if not status["completed"] or status["returncode"] != 0:
        raise ValueError(f"Incomplete simulation: {directory}")
    stats, blocks = read_stats(directory / "stats.txt")
    if manifest["warmup"] and blocks < 2:
        raise ValueError(f"Missing warmup statistics/reset: {directory}")
    if abs(stats["simInsts"] - manifest["measure"]) > 100:
        raise ValueError(f"Unexpected measured instruction count: {directory}")

    def total(pattern):
        return sum(
            value
            for name, value in stats.items()
            if re.fullmatch(pattern, name)
        )

    cycles = stats["system.cpu.numCycles"]
    result = {
        "instructions": stats["simInsts"],
        "cycles": cycles,
        "ipc": stats["simInsts"] / cycles,
        "l2_demand_misses": total(
            r"system\.(?:l2_wrappers\d*\.slices\d+\.inner_cache|l2_caches\d*)"
            r"\.demandMisses::total"
        ),
        "l3_demand_misses": stats.get("system.l3.demandMisses::total", 0),
        "l2_cpu_data_misses": total(
            r"system\.(?:l2_wrappers\d*\.slices\d+\.inner_cache|l2_caches\d*)"
            r"\.demandMisses::cpu\d*\.data"
        ),
        "l2_cpu_data_mshr_misses": total(
            r"system\.(?:l2_wrappers\d*\.slices\d+\.inner_cache|l2_caches\d*)"
            r"\.demandMshrMisses::cpu\d*\.data"
        ),
        "l2_prefetch_requestor_demand_misses": total(
            r"system\.(?:l2_wrappers\d*\.slices\d+\.inner_cache|l2_caches\d*)"
            r"\.demandMisses::cpu\d*\.dcache\.prefetcher"
        ),
        "wall_seconds": status["wall_seconds"],
    }
    for name in (
        "lookups",
        "eligibleAccesses",
        "excludedAccesses",
        "noPcAccesses",
        "eligibleMisses",
        "samplerAccesses",
        "samplerHits",
        "samplerEvictions",
        "liveTraining",
        "deadTraining",
        "samplerDeadHits",
        "samplerDeadEvictions",
        "predictionQueries",
        "deadPredictions",
        "deadHits",
        "fills",
        "invalidVictims",
        "deadVictims",
        "nonLruDeadVictims",
        "lruVictims",
        "bypasses",
    ):
        result[name] = total(r".*\.replacement_policy\." + name)
    result["l2_mpki"] = (
        1000 * result["l2_demand_misses"] / result["instructions"]
    )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.directory / "manifest.json").read_text())
    if "lru" not in manifest["variants"]:
        parser.error(
            "Comparison requires an lru variant in the same manifest; "
            "use metrics() to inspect a standalone diagnostic run"
        )
    rows = []
    for point in manifest["checkpoints"]:
        measurements = {}
        for variant in manifest["variants"]:
            directory = (
                args.directory / f"{point.replace('/', '_')}__{variant}"
            )
            measurements[variant] = metrics(directory, manifest)
        baseline = measurements["lru"]
        for variant, values in measurements.items():
            row = {"checkpoint": point, "variant": variant, **values}
            row["speedup"] = row["ipc"] / baseline["ipc"]
            row["l2_miss_change_pct"] = (
                100
                * (row["l2_demand_misses"] / baseline["l2_demand_misses"] - 1)
                if baseline["l2_demand_misses"]
                else None
            )
            rows.append(row)
    with (args.directory / "comparison.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (args.directory / "comparison.json").write_text(
        json.dumps(rows, indent=2) + "\n"
    )
    print(
        "| Checkpoint | Policy | IPC | Speedup | L2 MPKI | L2 miss change | Non-LRU dead victims | Bypasses |"
    )
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        miss_change = row["l2_miss_change_pct"]
        delta = "n/a" if miss_change is None else f"{miss_change:+.2f}%"
        print(
            f"| {row['checkpoint']} | {row['variant']} | {row['ipc']:.4f} | "
            f"{row['speedup']:.4f} | {row['l2_mpki']:.3f} | {delta} | "
            f"{row['nonLruDeadVictims']:.0f} | {row['bypasses']:.0f} |"
        )
    print(
        "\nUnweighted geometric mean speedup across these checkpoint points:"
    )
    for variant in manifest["variants"]:
        speedups = [
            row["speedup"] for row in rows if row["variant"] == variant
        ]
        mean = math.exp(sum(map(math.log, speedups)) / len(speedups))
        print(f"{variant}: {mean:.6f} ({100 * (mean - 1):+.3f}%)")
    print("This subset statistic is not a weighted SPEC benchmark score.")


if __name__ == "__main__":
    main()
