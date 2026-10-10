#!/usr/bin/env python3
"""Run self-checking workloads through the production Kunminghu Decode stage."""

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import time


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
COMPACTION = re.compile(
    r"Compaction: raw=(\d+) discarded=(\d+) fused=(\d+) "
    r"output=(\d+) queued=(\d+) stop=(\d+)"
)
STAT_NAMES = (
    "compactionRawInsts", "compactionDiscardedInsts", "compactionFusedPairs",
    "compactionOutputInsts", "compactionInputInsts", "compactionFlushedInsts",
    "compactionCrossBundleFusions", "compactionFetchBlockedCycles",
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def execute(command, directory, timeout):
    """Record the exact argv and log, including failed simulator invocations."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    started = time.monotonic()
    with (directory / "sim.log").open("w") as log:
        result = subprocess.run(
            command, cwd=REPO, stdout=log, stderr=subprocess.STDOUT,
            timeout=timeout, check=False,
        )
    return result.returncode, time.monotonic() - started


def read_stats(path):
    """Select the last completed dump; warmup is disabled for these fixtures."""
    completed = None
    current = None
    for line in path.read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            current = {}
        elif "End Simulation Statistics" in line:
            completed = current
            current = None
        elif current is not None:
            fields = line.split()
            if len(fields) >= 2:
                try:
                    current[fields[0]] = float(fields[1])
                except ValueError:
                    pass
    require(completed is not None, f"No completed stats dump in {path}")
    return completed


def stat(stats, name):
    found = [value for key, value in stats.items()
             if key == name or key.endswith(".decode." + name)]
    require(len(found) == 1, f"Expected one stat {name}, found {len(found)}")
    return found[0]


def validate_output(directory, enabled, trace=False, scan=16, capacity=40):
    log = (directory / "sim.log").read_text(errors="replace")
    if trace:
        require(re.search(r"Exiting @ tick \d+ because Trace-driven CPU .*"
                          r"(?:drained|traced instruction)", log),
                f"Trace did not exit normally: {directory / 'sim.log'}")
    else:
        require("because m5_exit instruction encountered" in log,
                f"Fixture did not reach success: {directory / 'sim.log'}")
    require("Simulated exit code not 0" not in log,
            f"Fixture returned failure: {directory / 'sim.log'}")
    stats = read_stats(directory / "stats.txt")
    debug_path = directory / "decode.log"
    samples = [tuple(map(int, match.groups()))
               for match in COMPACTION.finditer(debug_path.read_text())]
    values = {name: stat(stats, name) for name in STAT_NAMES}
    if not enabled or trace:
        require(not samples, "Legacy path unexpectedly executed compaction")
        require(all(value == 0 for value in values.values()),
                f"Legacy path updated compaction statistics: {values}")
    else:
        require(samples, "No production compaction samples were recorded")
        for raw, discarded, fused, output, queued, stop in samples:
            require(raw + discarded <= scan, "Exceeded raw scanning budget")
            require(output <= 8, "Exceeded Rename output width")
            require(output == raw - fused, "Incorrect raw/fused/output balance")
            require(2 * fused <= raw, "Fused an input more than once")
            require(queued <= capacity, "Exceeded Decode FIFO capacity")
        require(values["compactionRawInsts"] > 0, "Compaction never consumed input")
        require(values["compactionRawInsts"] - values["compactionFusedPairs"] ==
                values["compactionOutputInsts"], "Stats violate output balance")
        # Each successful fixture contains real fusion candidates. A result
        # with zero fusion would not exercise the behavior being changed.
        require(values["compactionFusedPairs"] > 0, "Fixture did not exercise fusion")
        for column, name in enumerate(("compactionRawInsts",
                                       "compactionDiscardedInsts",
                                       "compactionFusedPairs",
                                       "compactionOutputInsts")):
            require(sum(row[column] for row in samples) == values[name],
                    f"Per-cycle samples disagree with {name}")
    # ArchDB table_cmds contain unindented SQL newlines, so gem5's config.ini
    # is not always accepted by Python's general-purpose ConfigParser.
    configured = re.findall(r"^enableDecodeFusionCompaction=(true|false)$",
                            (directory / "config.ini").read_text(), re.MULTILINE)
    require(len(configured) == 1, "Expected one CPU with the compaction parameter")
    require((configured[0] == "true") == enabled,
            "The requested compaction parameter did not reach the CPU")
    if enabled and not trace:
        remaining = (values["compactionInputInsts"] - values["compactionRawInsts"] -
                     values["compactionDiscardedInsts"] - values["compactionFlushedInsts"])
        require(samples and remaining == samples[-1][4],
                "Received/consumed/discarded/flushed entries do not conserve FIFO occupancy")
    summary = {
        "stats": values,
        "simTicks": stats["simTicks"],
        "simInsts": stats["simInsts"],
        "simOps": stats["simOps"],
        "cycles_sampled": len(samples),
        "max_raw_per_cycle": max((row[0] for row in samples), default=0),
        "max_fused_per_cycle": max((row[2] for row in samples), default=0),
        "observed_16_raw_8_pairs": any(
            row[0] == 16 and row[2] == 8 and row[3] == 8 for row in samples),
        "observed_discard": any(row[1] > 0 for row in samples),
        "observed_backend_block": any(row[5] == 6 for row in samples),
        "observed_redirect": any(row[5] == 5 for row in samples),
        "observed_squash": any(row[5] == 7 for row in samples),
        "observed_scan_limit": any(row[5] == 2 for row in samples),
        "observed_cross_bundle_fusion": values["compactionCrossBundleFusions"] > 0,
    }
    return stats, summary


def check_equal(left, right, label):
    """Off and fallback comparisons use simulated work and time, not wall time."""
    keys = {"simTicks", "simInsts", "simOps"}
    keys.update(key for key in left
                if key.endswith(".numCycles") or ".committedInsts" in key or
                ".committedOps" in key or ".decode." in key)
    # New counters do not exist in an unmodified baseline binary.
    keys = {key for key in keys if "compaction" not in key}
    mismatches = {key: [left[key], right.get(key)] for key in sorted(keys)
                  if left[key] != right.get(key) and not
                  (math.isnan(left[key]) and math.isnan(right.get(key, 0)))}
    require(not mismatches, f"{label} differs: {mismatches}")


def build_fixtures(directory, prefix):
    images = {}
    for case, definition in (("scalar", None), ("control", "TEST_CONTROL"),
                             ("vector", "TEST_VECTOR")):
        obj = directory / f"{case}.o"
        elf = directory / f"{case}.elf"
        image = directory / f"{case}.bin"
        command = [prefix + "as", "-march=rv64gcv", "-o", str(obj)]
        if definition:
            command += ["--defsym", definition + "=1"]
        command += [str(HERE / "fusion.S")]
        subprocess.run(command, check=True)
        subprocess.run([prefix + "ld", "-Ttext=0x80000000", "-o", str(elf),
                        str(obj)], check=True)
        subprocess.run([prefix + "objcopy", "-O", "binary", str(elf), str(image)],
                       check=True)
        images[case] = image
    return images


def write_trace(path):
    # The reader's native ChampSim record is 64 bytes on the supported Linux
    # hosts: PC, branch bytes, six register bytes, and six memory addresses.
    record = struct.Struct("<Q8B6Q")
    with path.open("wb") as stream:
        for index in range(512):
            stream.write(record.pack(0x80000000 + index * 4, *([0] * 14)))


def run_tools(directory, timeout):
    require("O3PipeView:fetch:" in (directory / "decode.log").read_text(),
            "No O3PipeView records; use a tracing-enabled gem5.opt/debug binary")
    subprocess.run([
        sys.executable, str(REPO / "util/o3-pipeview.py"),
        "--cycle-time=333", "-o", str(directory / "pipeview.txt"),
        str(directory / "decode.log"),
    ], check=True, timeout=timeout)
    require((directory / "pipeview.txt").stat().st_size > 200,
            "O3PipeView produced no instruction timeline")
    db_path = directory / "lifetime.db"
    with sqlite3.connect(f"file:{db_path}?mode=ro", uri=True) as db:
        count = db.execute("SELECT count(*) FROM LifeTimeCommitTrace "
                           "WHERE AtCommit != 0").fetchone()[0]
    require(count > 0, "PerfCCT contains no committed instructions")
    with (directory / "perfcct.txt").open("w") as output:
        subprocess.run([
            sys.executable, str(REPO / "util/perfcct.py"), str(db_path), "--tid=0",
        ], stdout=output, check=True, timeout=timeout)
    require((directory / "perfcct.txt").stat().st_size > 0,
            "PerfCCT parser produced no output")
    return {"perfcct_committed_rows": count}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gem5", type=Path, required=True)
    parser.add_argument("--baseline-gem5", type=Path,
                        help="Optional unmodified binary built from the same base")
    parser.add_argument("--outdir", type=Path, required=True,
                        help="New directory; existing results are never overwritten")
    parser.add_argument("--ref-so", type=Path,
                        default=Path(os.environ["GCBV_REF_SO"])
                        if "GCBV_REF_SO" in os.environ else None)
    parser.add_argument("--no-difftest", action="store_true",
                        help="Self-check only; explicitly records missing difftest")
    parser.add_argument("--toolchain-prefix", default="riscv64-linux-gnu-")
    parser.add_argument("--cases", nargs="+", choices=("scalar", "control", "vector"),
                        default=["scalar", "control", "vector"])
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--mem-type", default="DDR3_1600_8x8",
                        help="Portable test memory model; not a SPEC score configuration")
    parser.add_argument("--tools", action="store_true",
                        help="Also check O3PipeView and PerfCCT with the scalar fixture")
    parser.add_argument("--skip-trace", action="store_true")
    parser.add_argument("--skip-rejections", action="store_true")
    parser.add_argument("--skip-bounds", action="store_true")
    parser.add_argument("--require-16-pairs", action="store_true",
                        help="Fail if no real cycle consumes 16 raw as 8 pairs")
    args = parser.parse_args()
    args.gem5 = args.gem5.resolve()
    args.outdir = args.outdir.resolve()
    require(args.gem5.is_file(), f"Missing simulator {args.gem5}")
    require(not args.outdir.exists(), f"Output directory already exists: {args.outdir}")
    if not args.no_difftest:
        require(args.ref_so is not None and args.ref_so.is_file(),
                "Provide --ref-so or GCBV_REF_SO; --no-difftest is self-check only")
        args.ref_so = args.ref_so.resolve()
    for tool in ("as", "ld", "objcopy"):
        require(shutil.which(args.toolchain_prefix + tool), f"Missing cross tool {tool}")
    args.outdir.mkdir(parents=True)
    report = {"difftest": not args.no_difftest, "runs": {}, "status": "running"}
    report_path = args.outdir / "report.json"
    try:
        images = build_fixtures(args.outdir, args.toolchain_prefix)
        common = [str(REPO / "configs/example/kmhv3.py"), "--mem-size=256MB",
                  "--mem-type=" + args.mem_type,
                  "--warmup-insts-no-switch=0", "--maxinsts=20000"]
        diff_args = (["--disable-difftest"] if args.no_difftest else
                     ["--enable-difftest", "--difftest-ref-so=" + str(args.ref_so)])

        def invoke(name, enabled, image=None, extra=(), trace=False,
                   binary=None, old=False, tools=False, scan=16, capacity=40):
            directory = args.outdir / name
            command = [str(binary or args.gem5), "--outdir=" + str(directory),
                       "--debug-file=decode.log", "--debug-flags=" +
                       ("Decode,O3PipeView" if tools else "Decode")]
            command += common
            if trace:
                command += ["--enable-trace-mode", "--trace-format=champsim",
                            "--trace-file=" + str(trace_path), "--trace-enable-decoupled-bp",
                            "--disable-difftest"]
            else:
                command += ["--raw-cpt", "--generic-rv-cpt=" + str(image)] + diff_args
            if not old:
                command += ["--param=system.cpu[0].enableDecodeFusionCompaction=" +
                            str(enabled)]
            if tools:
                command += ["--enable-arch-db", "--arch-db-dump-lifetime",
                            "--arch-db-file=" + str(directory / "lifetime.db")]
            command += list(extra)
            code, duration = execute(command, directory, args.timeout)
            require(code == 0, f"gem5 returned {code}: {directory / 'sim.log'}")
            if old:
                log = (directory / "sim.log").read_text(errors="replace")
                require("because m5_exit instruction encountered" in log and
                        "Simulated exit code not 0" not in log,
                        f"Baseline fixture failed: {directory / 'sim.log'}")
                stats = read_stats(directory / "stats.txt")
                summary = {key: stats[key] for key in ("simTicks", "simInsts", "simOps")}
            else:
                stats, summary = validate_output(directory, enabled, trace, scan, capacity)
            if tools:
                summary.update(run_tools(directory, args.timeout))
            summary["wall_seconds"] = round(duration, 3)
            report["runs"][name] = summary
            report_path.write_text(json.dumps(report, indent=2) + "\n")
            print(f"PASS {name}: {summary['simInsts']:.0f} instructions", flush=True)
            return stats

        for case in args.cases:
            extra = ["--enable-riscv-vector"] if case == "vector" else []
            off = invoke(case + "-false", False, images[case], extra=extra)
            on = invoke(case + "-true", True, images[case], extra=extra)
            require(off["simInsts"] == on["simInsts"],
                    f"{case}: False/True retired different architectural work")
            if args.baseline_gem5:
                baseline = invoke(case + "-baseline", False, images[case], extra=extra,
                                  binary=args.baseline_gem5.resolve(), old=True)
                check_equal(baseline, off, case + " baseline/False")

        if "control" in args.cases:
            extra = ["--param=system.cpu[0].enablePredecode=False"]
            off = invoke("control-no-predecode-false", False, images["control"], extra=extra)
            on = invoke("control-no-predecode-true", True, images["control"], extra=extra)
            require(off["simInsts"] == on["simInsts"],
                    "Control without predecode retired different architectural work")
            require(report["runs"]["control-no-predecode-true"]["observed_redirect"],
                    "Control fixture without predecode never exercised Decode selfSquash")
        if not args.skip_bounds:
            for scan, capacity in ((8, 40), (16, 24)):
                invoke(f"bounds-scan{scan}-capacity{capacity}", True, images["scalar"],
                       extra=[f"--param=system.cpu[0].decodeFusionScanWidth={scan}",
                              f"--param=system.cpu[0].decodeFusionBufferSize={capacity}"],
                       scan=scan, capacity=capacity)
        if not args.skip_trace:
            trace_path = args.outdir / "nop.champsim"
            write_trace(trace_path)
            off = invoke("trace-false", False, trace=True)
            on = invoke("trace-true-fallback", True, trace=True)
            check_equal(off, on, "Trace False/True fallback")
        if not args.skip_rejections:
            rejections = (
                ("enable_loadFusion", "True", "enable_loadFusion"),
                ("enableConstantFolding", "True", "enableConstantFolding"),
                ("enableMovImmElimination", "True", "enableMovImmElimination"),
                ("fetchToDecodeDelay", "2", "fetchToDecodeDelay"),
                ("decodeFusionBufferSize", "23", "decodeFusionBufferSize"),
                ("decodeFusionScanWidth", "0", "decodeFusionScanWidth"),
            )
            for parameter, value, diagnostic in rejections:
                directory = args.outdir / ("reject-" + parameter)
                command = [str(args.gem5), "--outdir=" + str(directory)] + common
                command += ["--raw-cpt", "--generic-rv-cpt=" + str(images["scalar"]),
                            "--disable-difftest",
                            "--param=system.cpu[0].enableDecodeFusionCompaction=True",
                            f"--param=system.cpu[0].{parameter}={value}"]
                code, duration = execute(command, directory, args.timeout)
                log = (directory / "sim.log").read_text(errors="replace")
                require(code != 0 and re.search(r"(?:fatal|panic):.*" +
                        re.escape(diagnostic), log),
                        f"Wrong/missing rejection for {parameter}: {directory / 'sim.log'}")
                report["runs"][directory.name] = {"rejected_parameter": parameter}
                print(f"PASS reject-{parameter}", flush=True)
        if args.tools:
            for enabled in (False, True):
                invoke("tools-" + str(enabled).lower(), enabled, images["scalar"], tools=True)
        if args.require_16_pairs:
            require(any(run.get("observed_16_raw_8_pairs", False)
                        for run in report["runs"].values()),
                    "No 16-raw/8-pair cycle observed; do not claim this boundary covered")
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Report: {report_path}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
