#!/usr/bin/env python3

import argparse
import math
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
WORKLOAD_DIR = ROOT / "tests" / "test-progs" / "partial-store-perf"

STAT_KEYS = {
    "sim_cycles": "system.cpu.numCycles",
    "committed_insts": "system.cpu.committedInsts",
    "ipc": "system.cpu.ipc",
    "permission_reqs": "system.cpu.dcache.partialPermissionReqs",
    "partial_write_reqs": "system.cpu.lsq.sbufferPartialWriteReqs",
    "partial_write_ddr_reqs": "system.cpu.lsq.sbufferPartialWriteDdrReqs",
    "partial_writebacks": "system.cpu.dcache.partialLineWritebacks",
    "cpu_data_bytes_read": "system.mem_ctrls.bytesRead::cpu.data",
    "store_perm_xbar": "system.tol2bus_list.transDist::StorePermReq",
    "read_ex_xbar": "system.tol2bus_list.transDist::ReadExReq",
}

ROI_RE = re.compile(
    r"partial-store-perf: cycles=(\d+) instructions=(\d+) stores=(\d+)"
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build and compare partial-store off/on gem5 runs")
    parser.add_argument("--am-home", type=Path,
                        help="nexus-am root; defaults to AM_HOME or ../nexus-am")
    parser.add_argument("--gem5", type=Path,
                        default=ROOT / "build" / "RISCV" / "gem5.opt")
    parser.add_argument("--lines", type=int, default=65536)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--stride", type=int, default=8191)
    parser.add_argument("--out-root", type=Path,
                        default=ROOT / "results" / "partial-store-perf")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--extra-gem5-arg", action="append", default=[])
    return parser.parse_args()


def resolve_am_home(argument):
    candidate = argument
    if candidate is None and os.environ.get("AM_HOME"):
        candidate = Path(os.environ["AM_HOME"])
    if candidate is None:
        candidate = ROOT.parent / "nexus-am"
    candidate = candidate.expanduser().resolve()
    if not (candidate / "Makefile.app").is_file():
        raise SystemExit(f"invalid nexus-am path: {candidate}")
    return candidate


def validate_args(args):
    if args.lines < 16 or args.lines & (args.lines - 1):
        raise SystemExit("--lines must be a power of two and at least 16")
    if args.rounds <= 0:
        raise SystemExit("--rounds must be positive")
    if args.stride <= 0 or args.stride % 2 == 0:
        raise SystemExit("--stride must be a positive odd number")


def workload_binary(args):
    config = f"{args.lines}-{args.rounds}-{args.stride}"
    return WORKLOAD_DIR / "build" / f"partial-store-perf-{config}-riscv64-xs.bin"


def build_workload(args, am_home):
    command = [
        "make", "-C", str(WORKLOAD_DIR),
        "ARCH=riscv64-xs", "LINUX_GNU_TOOLCHAIN=1",
        f"AM_HOME={am_home}", f"PERF_NUM_LINES={args.lines}",
        f"PERF_ROUNDS={args.rounds}", f"PERF_LINE_STRIDE={args.stride}",
    ]
    subprocess.run(command, check=True)


def run_gem5(args, binary, mode):
    outdir = args.out_root.resolve() / mode
    outdir.mkdir(parents=True, exist_ok=True)
    log_path = outdir / "run.log"
    command = [
        str(args.gem5.resolve()), f"--outdir={outdir}",
        str(ROOT / "configs" / "example" / "kmhv3.py"),
        "--raw-cpt", f"--generic-rv-cpt={binary}", "--disable-difftest",
        f"--{mode}-partial-store",
        *args.extra_gem5_arg,
    ]
    print(f"Running partial store {mode}: {' '.join(command)}")
    with log_path.open("w", encoding="utf-8") as log:
        subprocess.run(command, cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    return outdir


def parse_stats(path):
    values = {}
    with path.open(encoding="utf-8") as stats:
        for line in stats:
            fields = line.split()
            if len(fields) >= 2 and fields[0] in STAT_KEYS.values():
                try:
                    values[fields[0]] = float(fields[1])
                except ValueError:
                    pass
    return {name: values.get(key, 0.0) for name, key in STAT_KEYS.items()}


def parse_result(outdir):
    stats_path = outdir / "stats.txt"
    log_path = outdir / "run.log"
    if not stats_path.is_file():
        raise SystemExit(f"missing stats file: {stats_path}")
    log = log_path.read_text(encoding="utf-8", errors="replace")
    match = ROI_RE.search(log)
    if not match or "partial-store-perf: PASS" not in log:
        raise SystemExit(f"workload did not complete successfully; see {log_path}")
    result = parse_stats(stats_path)
    result.update({
        "roi_cycles": float(match.group(1)),
        "roi_insts": float(match.group(2)),
        "stores": float(match.group(3)),
    })
    return result


def format_value(value):
    if math.isfinite(value) and value == int(value):
        return str(int(value))
    return f"{value:.4f}"


def report(baseline, partial):
    rows = [
        ("ROI cycles", "roi_cycles"),
        ("ROI instructions", "roi_insts"),
        ("Simulation cycles", "sim_cycles"),
        ("IPC", "ipc"),
        ("Partial write requests", "partial_write_reqs"),
        ("L1D permission requests", "permission_reqs"),
        ("StorePermReq at tol2bus", "store_perm_xbar"),
        ("ReadExReq at tol2bus", "read_ex_xbar"),
        ("Partial writes reaching DDR", "partial_write_ddr_reqs"),
        ("CPU data bytes read", "cpu_data_bytes_read"),
        ("Partial line writebacks", "partial_writebacks"),
    ]
    print("\nPartial-store A/B result")
    print(f"{'Metric':32} {'disabled':>14} {'enabled':>14}")
    print("-" * 62)
    for label, key in rows:
        print(f"{label:32} {format_value(baseline[key]):>14} "
              f"{format_value(partial[key]):>14}")

    speedup = baseline["roi_cycles"] / partial["roi_cycles"]
    reduction = 1.0 - partial["roi_cycles"] / baseline["roi_cycles"]
    print(f"\nROI speedup: {speedup:.4f}x ({reduction * 100:.2f}% fewer cycles)")

    errors = []
    if baseline["store_perm_xbar"] != 0:
        errors.append("disabled run unexpectedly issued StorePermReq")
    if partial["store_perm_xbar"] == 0:
        errors.append("enabled run did not issue StorePermReq")
    if partial["roi_cycles"] >= baseline["roi_cycles"]:
        errors.append("enabled run did not reduce ROI cycles")
    if partial["cpu_data_bytes_read"] >= baseline["cpu_data_bytes_read"]:
        errors.append("enabled run did not reduce CPU data memory reads")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


def main():
    args = parse_args()
    validate_args(args)
    am_home = resolve_am_home(args.am_home)
    args.gem5 = args.gem5.expanduser()
    if not args.gem5.is_file():
        raise SystemExit(f"gem5 binary not found: {args.gem5}")

    binary = workload_binary(args)
    if not args.skip_build:
        build_workload(args, am_home)
    if not binary.is_file():
        raise SystemExit(f"workload binary not found: {binary}")

    baseline_dir = run_gem5(args, binary.resolve(), "disable")
    partial_dir = run_gem5(args, binary.resolve(), "enable")
    return report(parse_result(baseline_dir), parse_result(partial_dir))


if __name__ == "__main__":
    sys.exit(main())
