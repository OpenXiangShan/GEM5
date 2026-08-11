#!/usr/bin/env python3
"""Prepare, run, and score selected H-mode SPEC06 checkpoint sets."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path


CLOCK_RATE = 3_000_000_000

FIXED_H_ARGUMENTS = (
    "--enable-h-gcpt",
    "--restore-rvh-cpt",
    "--gcpt-restorer",
    "--generic-rv-cpt",
    "--difftest-ref-so",
    "--maxinsts",
    "--mem-type",
    "--dramsim3-ini",
    "--pf-control-profile",
)

SPEC06_INT = [
    "perlbench", "bzip2", "gcc", "mcf", "gobmk", "hmmer", "sjeng",
    "libquantum", "h264ref", "omnetpp", "astar", "xalancbmk",
]

SPEC06_FP = [
    "bwaves", "gamess", "milc", "zeusmp", "gromacs", "cactusADM",
    "leslie3d", "namd", "dealII", "soplex", "povray", "calculix",
    "GemsFDTD", "tonto", "lbm", "wrf", "sphinx3",
]

SPEC06_REFTIME = {
    "perlbench": 9770.0,
    "bzip2": 9650.0,
    "gcc": 8050.0,
    "mcf": 9120.0,
    "gobmk": 10490.0,
    "hmmer": 9330.0,
    "sjeng": 12100.0,
    "libquantum": 20720.0,
    "h264ref": 22130.0,
    "omnetpp": 6250.0,
    "astar": 7020.0,
    "xalancbmk": 6900.0,
    "bwaves": 13590.0,
    "gamess": 19580.0,
    "milc": 9180.0,
    "zeusmp": 9100.0,
    "gromacs": 7140.0,
    "cactusADM": 11950.0,
    "leslie3d": 9400.0,
    "namd": 8020.0,
    "dealII": 11440.0,
    "soplex": 8340.0,
    "povray": 5320.0,
    "calculix": 8250.0,
    "GemsFDTD": 10610.0,
    "tonto": 9840.0,
    "lbm": 13740.0,
    "wrf": 11170.0,
    "sphinx3": 19490.0,
}


def load_selection(path: Path) -> dict:
    with path.open() as source:
        data = json.load(source)
    if not isinstance(data, dict):
        raise ValueError(f"selection JSON must be an object: {path}")
    return data


def filters(raw: str) -> list[str]:
    return [token.strip().lower() for token in raw.split(",") if token.strip()]


def selected_workloads(data: dict, raw_filter: str) -> list[str]:
    requested = filters(raw_filter)
    names = sorted(data)
    if not requested:
        return names
    selected = [
        workload for workload in names
        if any(token in workload.lower() for token in requested)
    ]
    if not selected:
        raise ValueError(f"benchmark filter matched no H workloads: {raw_filter}")
    return selected


def points_for(data: dict, workloads: list[str]):
    for workload in workloads:
        metadata = data[workload]
        insts = float(metadata.get("insts", 0))
        if insts <= 0:
            raise ValueError(
                f"H selection has no instruction count for {workload}; "
                "use the H-profile-filled selection JSON"
            )
        points = metadata.get("points", {})
        if not points:
            raise ValueError(f"H selection has no points for {workload}")
        for point, weight in sorted(points.items(), key=lambda item: int(item[0])):
            yield workload, str(point), float(weight), insts


def command_make_list(args: argparse.Namespace) -> int:
    if args.max_tasks < 0:
        raise ValueError("--max-tasks must be non-negative")
    data = load_selection(args.json)
    workloads = selected_workloads(data, args.benchmarks)
    points = list(points_for(data, workloads))
    if args.max_tasks:
        points = points[:args.max_tasks]
    if args.expect_checkpoints and len(points) != args.expect_checkpoints:
        raise ValueError(
            f"expected {args.expect_checkpoints} checkpoints, got {len(points)}"
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as output:
        for workload, point, _weight, _insts in points:
            output.write(f"{workload}_{point} {workload}/{point} 0 0 20 20\n")

    print(
        f"Generated {len(points)} H checkpoint entries from {len(workloads)} "
        f"workloads: {args.output}"
    )
    return 0


def ensure_file(path: Path, description: str) -> None:
    if not path.is_file():
        raise ValueError(f"{description} is not a readable file: {path}")


def reject_fixed_h_overrides(extra_args: str) -> None:
    for token in shlex.split(extra_args):
        if any(token == fixed or token.startswith(f"{fixed}=")
               for fixed in FIXED_H_ARGUMENTS):
            raise ValueError(
                f"extra arguments must not override fixed H option: {token}"
            )


def command_run(args: argparse.Namespace) -> int:
    ensure_file(args.config, "H GEM5 configuration")
    ensure_file(args.checkpoint_list, "H checkpoint list")
    if not args.checkpoint_root.is_dir():
        raise ValueError(
            f"H checkpoint root is not a directory: {args.checkpoint_root}"
        )
    ensure_file(args.ref_so, "H NEMU reference")
    ensure_file(args.restorer, "H checkpoint restorer")
    ensure_file(args.dram_ini, "DRAMsim3 configuration")
    if args.maxinsts <= 0:
        raise ValueError("--maxinsts must be positive")
    extra_args = args.extra_args.replace("${GEM5_HOME}", str(args.gem5_home))
    extra_args = extra_args.replace("$GEM5_HOME", str(args.gem5_home))
    reject_fixed_h_overrides(extra_args)

    fixed_args = [
        "--enable-h-gcpt",
        "--restore-rvh-cpt",
        f"--gcpt-restorer={args.restorer}",
        f"--difftest-ref-so={args.ref_so}",
        f"--maxinsts={args.maxinsts}",
        f"--dramsim3-ini={args.dram_ini}",
        f"--pf-control-profile={args.pf_control_profile}",
    ]
    gem5_args = shlex.join([*shlex.split(extra_args), *fixed_args])
    runner = Path(__file__).with_name("distributed_sim.py")
    ensure_file(runner, "distributed checkpoint runner")

    command = [
        sys.executable,
        str(runner),
        "--servers", args.servers or "local",
        "--jobs-per-server", str(args.jobs_per_server),
        "--build-type", args.build_type,
        "--gem5-home", str(args.gem5_home),
        "--env", f"GCBH_REF_SO={args.ref_so}",
        "--env", f"GCBH_RESTORER={args.restorer}",
    ]
    if args.max_tasks:
        command.extend(["--max-tasks", str(args.max_tasks)])
    if args.require_idle_cpus:
        command.extend(["--require-idle-cpus", str(args.require_idle_cpus)])
        command.extend(["--idle-probe-mode", args.idle_probe_mode])
    if args.dispatch_host:
        command.extend(["--dispatch-host", args.dispatch_host])
    for ssh_option in args.ssh_option:
        command.extend(["--ssh-option", ssh_option])
    command.extend([
        "--launch-retries", str(args.launch_retries),
        "--launch-retry-delay", str(args.launch_retry_delay),
        "--launch-interval", str(args.launch_interval),
        str(args.config),
        str(args.checkpoint_list),
        str(args.checkpoint_root),
        args.tag,
        args.benchmarks,
        gem5_args,
    ])

    print(f"H runner GEM5 arguments: {gem5_args}")
    print(f"H runner command: {shlex.join(command)}")
    env = os.environ.copy()
    env["GCBH_REF_SO"] = str(args.ref_so)
    env["GCBH_RESTORER"] = str(args.restorer)
    return subprocess.run(command, env=env, check=False).returncode


def parse_stats(path: Path) -> tuple[float, float, float]:
    sim_insts = None
    cycles = None
    ipc = None
    with path.open(errors="ignore") as stats:
        for line in stats:
            fields = line.split()
            if len(fields) < 2:
                continue
            if fields[0] == "simInsts":
                sim_insts = float(fields[1])
            elif fields[0] == "system.cpu.numCycles":
                cycles = float(fields[1])
            elif fields[0] == "system.cpu.ipc":
                ipc = float(fields[1])
    if ipc is None:
        if sim_insts is None or cycles is None or cycles <= 0:
            raise ValueError(f"cannot determine IPC from {path}")
        ipc = sim_insts / cycles
    if cycles is None:
        if sim_insts is None or ipc <= 0:
            raise ValueError(f"cannot determine cycles from {path}")
        cycles = sim_insts / ipc
    if sim_insts is None:
        sim_insts = 0.0
    if ipc <= 0:
        raise ValueError(f"invalid IPC in {path}: {ipc}")
    return sim_insts, cycles, ipc


def find_stats(task_dir: Path) -> Path | None:
    candidates = [
        task_dir / "m5out" / "stats.txt",
        task_dir / "stats.txt",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    matches = sorted(task_dir.glob("**/stats.txt"))
    return matches[0] if matches else None


def benchmark_name(workload: str) -> str:
    match = re.match(r"[A-Za-z0-9]+", workload)
    if not match:
        raise ValueError(f"cannot infer benchmark from workload {workload}")
    return match.group(0)


def geometric_mean(values: list[float]) -> float:
    values = [value for value in values if value > 0]
    if not values:
        return 0.0
    return math.exp(sum(math.log(value) for value in values) / len(values))


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("")
        return
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def command_score(args: argparse.Namespace) -> int:
    if args.max_tasks < 0:
        raise ValueError("--max-tasks must be non-negative")
    data = load_selection(args.json)
    workloads = selected_workloads(data, args.benchmarks)
    expected = list(points_for(data, workloads))
    if args.max_tasks:
        expected = expected[:args.max_tasks]
    status_rows = []
    completed = {}

    for workload, point, weight, insts in expected:
        task_name = f"{workload}_{point}"
        task_dir = args.result_dir / task_name
        stats_path = find_stats(task_dir)
        marker = ""
        reason = ""
        state = "MISSING"
        sim_insts = cycles = ipc = ""
        if (task_dir / "abort").exists():
            state = "FAILED"
            marker = "abort"
        elif (task_dir / "running").exists():
            state = "RUNNING"
            marker = "running"
        elif stats_path is None:
            reason = "stats.txt missing"
        else:
            try:
                sim_insts, cycles, ipc = parse_stats(stats_path)
                if (task_dir / "completed").exists():
                    state = "FINISHED"
                    marker = "completed"
                    completed[(workload, point)] = {
                        "weight": weight,
                        "insts": insts,
                        "ipc": ipc,
                    }
                else:
                    state = "INCOMPLETE"
                    reason = "stats.txt exists without completed marker"
            except ValueError as error:
                state = "FAILED"
                reason = str(error)

        status_rows.append({
            "workload": workload,
            "benchmark": benchmark_name(workload),
            "point": point,
            "weight": weight,
            "state": state,
            "marker": marker,
            "reason": reason,
            "stats": str(stats_path) if stats_path else "",
            "sim_insts": sim_insts,
            "cycles": cycles,
            "ipc": ipc,
        })

    workload_metrics = {}
    for workload in workloads:
        metadata = data[workload]
        insts = float(metadata["insts"])
        points = {str(point): float(weight) for point, weight in metadata["points"].items()}
        finished = [
            (point, completed[(workload, point)])
            for point in points if (workload, point) in completed
        ]
        coverage = sum(points[point] for point, _ in finished)
        if coverage <= 0:
            continue
        seconds = sum(
            points[point] / coverage * insts / entry["ipc"] / CLOCK_RATE
            for point, entry in finished
        )
        workload_metrics[workload] = {
            "benchmark": benchmark_name(workload),
            "insts": insts,
            "time": seconds,
            "coverage": coverage,
        }

    by_benchmark = {}
    for metrics in workload_metrics.values():
        by_benchmark.setdefault(metrics["benchmark"], []).append(metrics)

    score_rows = []
    for benchmark in SPEC06_INT + SPEC06_FP:
        entries = by_benchmark.get(benchmark, [])
        if not entries:
            continue
        total_insts = sum(entry["insts"] for entry in entries)
        seconds = sum(entry["time"] for entry in entries)
        coverage = sum(
            entry["coverage"] * entry["insts"] / total_insts
            for entry in entries
        )
        score_rows.append({
            "benchmark": benchmark,
            "suite": "Int" if benchmark in SPEC06_INT else "FP",
            "time_seconds": seconds,
            "ref_time_seconds": SPEC06_REFTIME[benchmark],
            "score_per_ghz": (SPEC06_REFTIME[benchmark] / seconds) / 3.0,
            "coverage": coverage,
        })

    int_score = geometric_mean([
        row["score_per_ghz"] for row in score_rows if row["suite"] == "Int"
    ])
    fp_score = geometric_mean([
        row["score_per_ghz"] for row in score_rows if row["suite"] == "FP"
    ])
    overall_score = geometric_mean([row["score_per_ghz"] for row in score_rows])
    failed_rows = [row for row in status_rows if row["state"] != "FINISHED"]

    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.out_dir / "checkpoint_status.csv", status_rows)
    write_csv(args.out_dir / "score_by_benchmark.csv", score_rows)
    summary = {
        "label": args.label,
        "expected_checkpoints": len(expected),
        "finished_checkpoints": len(expected) - len(failed_rows),
        "failed_checkpoints": len(failed_rows),
        "int_score_per_ghz": int_score,
        "fp_score_per_ghz": fp_score,
        "overall_score_per_ghz": overall_score,
    }
    (args.out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    lines = [
        f"================ {args.label} ================",
        "benchmark       suite   score/GHz  coverage",
    ]
    for row in score_rows:
        lines.append(
            f"{row['benchmark']:<15} {row['suite']:<5} "
            f"{row['score_per_ghz']:>10.3f} {row['coverage']:>9.3f}"
        )
    lines.extend([
        "",
        f"Estimated Int score per GHz: {int_score:.6f}",
        f"Estimated FP score per GHz: {fp_score:.6f}",
        f"Estimated overall score per GHz: {overall_score:.6f}",
        f"Checkpoints: {summary['finished_checkpoints']}/{summary['expected_checkpoints']} finished, "
        f"{summary['failed_checkpoints']} incomplete or failed",
    ])
    (args.out_dir / "score.txt").write_text("\n".join(lines) + "\n")
    return 0


def command_validate(args: argparse.Namespace) -> int:
    with args.summary.open() as source:
        summary = json.load(source)
    expected = int(summary["expected_checkpoints"])
    finished = int(summary["finished_checkpoints"])
    failed = int(summary["failed_checkpoints"])
    if expected == 0 or finished != expected or failed != 0:
        print(
            f"H SPEC06 validation failed: finished={finished}, "
            f"expected={expected}, failed={failed}",
            file=sys.stderr,
        )
        return 1
    print(f"H SPEC06 validation passed: {finished}/{expected} checkpoints")
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    list_parser = subparsers.add_parser("make-list")
    list_parser.add_argument("--json", type=Path, required=True)
    list_parser.add_argument("--output", type=Path, required=True)
    list_parser.add_argument("--benchmarks", default="")
    list_parser.add_argument("--expect-checkpoints", type=int, default=0)
    list_parser.add_argument("--max-tasks", type=int, default=0)
    list_parser.set_defaults(handler=command_make_list)

    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--config", type=Path, required=True)
    run_parser.add_argument("--checkpoint-list", type=Path, required=True)
    run_parser.add_argument("--checkpoint-root", type=Path, required=True)
    run_parser.add_argument("--tag", required=True)
    run_parser.add_argument("--benchmarks", default="")
    run_parser.add_argument("--servers", default="local")
    run_parser.add_argument("--jobs-per-server", type=int, default=1)
    run_parser.add_argument("--require-idle-cpus", type=int, default=0)
    run_parser.add_argument("--idle-probe-mode", default="physical",
                            choices=("physical", "logical"))
    run_parser.add_argument("--dispatch-host", default="")
    run_parser.add_argument("--ssh-option", action="append", default=[])
    run_parser.add_argument("--launch-retries", type=int, default=2)
    run_parser.add_argument("--launch-retry-delay", type=float, default=20.0)
    run_parser.add_argument("--launch-interval", type=float, default=0.2)
    run_parser.add_argument("--gem5-home", type=Path,
                            default=Path(os.environ.get("GEM5_HOME", Path(__file__).parents[2])))
    run_parser.add_argument("--build-type",
                            default=os.environ.get("GEM5_BUILD_TYPE", "opt"))
    run_parser.add_argument("--ref-so", type=Path,
                            default=Path(os.environ.get("GCBH_REF_SO", "")))
    run_parser.add_argument("--restorer", type=Path,
                            default=Path(os.environ.get("GCBH_RESTORER", "")))
    run_parser.add_argument("--maxinsts", type=int, default=40_000_000)
    run_parser.add_argument("--dram-ini", type=Path, required=True)
    run_parser.add_argument("--pf-control-profile", default="off",
                            choices=("off", "adaptive", "default"))
    run_parser.add_argument("--extra-args", default="")
    run_parser.add_argument("--max-tasks", type=int, default=0)
    run_parser.set_defaults(handler=command_run)

    score_parser = subparsers.add_parser("score")
    score_parser.add_argument("--json", type=Path, required=True)
    score_parser.add_argument("--result-dir", type=Path, required=True)
    score_parser.add_argument("--out-dir", type=Path, required=True)
    score_parser.add_argument("--benchmarks", default="")
    score_parser.add_argument("--max-tasks", type=int, default=0)
    score_parser.add_argument("--label", default="H SPEC06")
    score_parser.set_defaults(handler=command_score)

    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--summary", type=Path, required=True)
    validate_parser.set_defaults(handler=command_validate)
    return parser


def main() -> int:
    args = make_parser().parse_args()
    try:
        return args.handler(args)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
