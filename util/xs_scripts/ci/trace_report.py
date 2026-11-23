"""Summarize post-warmup trace IPC and branch MPKI."""

import json
import math
from pathlib import Path

from .benchmarks import BenchmarkProfile


def summarize_trace_results(
    config: BenchmarkProfile, stats_root: str | Path
) -> int:
    """Print measurement IPC/MPKI using only archived run outputs."""
    root = Path(stats_root)
    weights_path = None
    if config.weights:
        archived = root.parent / "cluster.json"
        weights_path = archived if archived.is_file() else config.weights
    aborted = sorted(root.rglob("abort"))
    failed = len(aborted)
    results = {}
    print(f"{config.benchmark_type}: measurement after warmup")
    print(f"{'Trace':<32} {'Status':<8} {'IPC':>12} {'Branch MPKI':>12}")
    for marker in aborted:
        print(f"{marker.parent.name:<32} failed: run aborted")
    # Inspect task directories even if a worker never wrote stats.txt.
    directories = sorted(
        p for p in root.glob("*") if p.is_dir() and not p.is_symlink()
    )
    if not directories:
        print("No trace task directories found")
        return 1
    for directory in directories:
        name = directory.name
        if (directory / "abort").exists():
            continue
        try:
            if (
                not (directory / "completed").is_file()
                or (directory / "running").exists()
            ):
                raise ValueError("run did not complete")
            stats_file = directory / "stats.txt"
            # The final complete dump contains post-warmup measurements.
            blocks, current = [], None
            for entry in stats_file.read_text().splitlines():
                if "Begin Simulation Statistics" in entry:
                    current = {}
                elif "End Simulation Statistics" in entry:
                    if current is not None:
                        blocks.append(current)
                    current = None
                elif current is not None:
                    fields = entry.split()
                    if fields and fields[0] in (
                        "simInsts",
                        "system.cpu.numCycles",
                        "system.cpu.commit.branchMispredicts",
                    ):
                        current[fields[0]] = float(fields[1])
            if not blocks:
                raise ValueError("missing statistics dump")
            stats = blocks[-1]
            insts = stats["simInsts"]
            cycles = stats["system.cpu.numCycles"]
            misses = stats["system.cpu.commit.branchMispredicts"]
            if (
                not all(math.isfinite(v) for v in (insts, cycles, misses))
                or insts <= 0
                or cycles <= 0
                or misses < 0
            ):
                raise ValueError("invalid measurement statistics")
            results[name] = (insts / cycles, 1000 * misses / insts)
            print(
                f"{name:<32} {'passed':<8} {insts / cycles:>12.6f} "
                f"{1000 * misses / insts:>12.6f}"
            )
        except (OSError, ValueError, KeyError, IndexError) as error:
            failed += 1
            print(f"{name:<32} failed: {error}")
    if not failed and results:
        try:
            groups = [dict.fromkeys(results, 1.0)]
            if weights_path:
                workloads = json.loads(Path(weights_path).read_text())[
                    "workloads"
                ]
                groups = [
                    {
                        name: float(point["rv64_weight"])
                        for name, point in points.items()
                        if name in results
                    }
                    for points in workloads.values()
                ]
                groups = [points for points in groups if points]
            weights = {}
            for points in groups:
                if any(
                    not math.isfinite(w) or w <= 0 for w in points.values()
                ):
                    raise ValueError("invalid trace weights")
                # Normalize selected slices, then weight workloads equally.
                total = sum(points.values()) * len(groups)
                weights.update({name: w / total for name, w in points.items()})
            if results.keys() - weights.keys():
                missing = sorted(results.keys() - weights.keys())
                raise ValueError(f"traces missing weights: {missing}")
            ipc = sum(weights[name] * row[0] for name, row in results.items())
            mpki = sum(weights[name] * row[1] for name, row in results.items())
            label = (
                "equal workload weights"
                if weights_path
                else "equal trace weights"
            )
            print(f"Overall IPC ({label}): {ipc:.6f}")
            print(f"Overall branch MPKI ({label}): {mpki:.6f}")
        except (OSError, ValueError, KeyError, TypeError) as error:
            failed += 1
            print(f"Aggregation failed: {error}")
    else:
        print("Overall averages unavailable: failed or incomplete runs")
    print(f"Failed traces: {failed}")
    return int(failed > 0)
