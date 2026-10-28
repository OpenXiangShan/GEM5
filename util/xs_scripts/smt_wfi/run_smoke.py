#!/usr/bin/env python3
"""Smoke-test software WFI looping using an unmodified CPU and multi-hart REF."""

import argparse
import json
import os
from pathlib import Path
import subprocess

from validate import read_stats, validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--restorer", type=Path, required=True)
    parser.add_argument("--ref-so", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=Path("build/RISCV/gem5.fast"))
    parser.add_argument("--case", choices=["loop", "trace", "normal", "timeout"],
                        default="loop")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cmd = [str(args.binary.resolve()), "--outdir=" + str(out),
           str(repo / "configs/example/smt_idealkmhv3.py"),
           "--generic-rv-cpt=" + str(args.checkpoint.resolve()),
           "--gcpt-restorer=" + str(args.restorer.resolve()),
           "--difftest-ref-so=" + str(args.ref_so.resolve()), "--mem-size=16GB",
           "--abs-max-tick=10000000000"]
    if args.case == "normal":
        cmd = [s for s in cmd if not s.startswith("--gcpt-restorer=")]
        cmd += ["--gcpt-restorer=", "--maxinsts=1200000"]
    else:
        cmd += ["--smt-wfi-loop-test", "--warmup-insts-no-switch=1300000",
                "--maxinsts=1800000"]
        if args.case == "trace":
            cmd += ["--warmup-insts-no-switch=50000", "--maxinsts=100000",
                    "--smt-wfi-loop-restore-insts=50000", "--dump-commit",
                    "--dump-start=100000"]
        elif args.case == "timeout":
            cmd += ["--abs-max-tick=1000"]
    (out / "command.json").write_text(json.dumps(cmd, indent=2) + "\n")
    env = os.environ.copy()
    env["LLVM_PROFILE_FILE"] = str(out / "default.profraw")
    with (out / "run.log").open("w") as log:
        result = subprocess.run(cmd, cwd=repo, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=300)
    if args.case == "timeout":
        text = (out / "run.log").read_text(errors="replace")
        if result.returncode == 0 or "WFI loop expected" not in text:
            raise RuntimeError("timeout did not reject incomplete measurement")
    elif result.returncode != 0:
        raise RuntimeError(f"gem5 failed: see {out / 'run.log'}")
    elif args.case == "normal":
        stats = read_stats(out / "stats.txt")[-1]
        if min(stats[f"system.cpu.committedInsts::{t}"] for t in range(2)) <= 0:
            raise RuntimeError("normal A+A lost thread progress")
    else:
        report, stats = validate(out)
        if args.case == "trace":
            pc = report["wfi_pc"]
            lines = (out / "dumpCommit.txt").read_text().splitlines()
            wfi = [s for s in lines if s.startswith(f"{pc:x} ")]
            jumps = [s for s in lines if s.startswith(f"{pc + 4:x} ")]
            if min(len(wfi), len(jumps)) < 10:
                raise RuntimeError("trace does not show repeated WFI and JAL")
            (out / "loop-trace-summary.json").write_text(json.dumps(
                {"wfi_commits": len(wfi), "jump_commits": len(jumps),
                 "sample": (wfi[:2] + jumps[:2])}, indent=2) + "\n")
    (out / "test-result.json").write_text(json.dumps(
        {"case": args.case, "passed": True, "gem5_exit_code": result.returncode},
        indent=2) + "\n")
    print(f"PASS {args.case}: {out}")


if __name__ == "__main__":
    main()
