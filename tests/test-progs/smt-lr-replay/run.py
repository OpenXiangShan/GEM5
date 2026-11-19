#!/usr/bin/env python3
"""Run bare-metal SMT LR tests, requiring guest completion and zero errors."""

import argparse
import json
import os
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent
repo = root.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--gem5", type=Path, default=repo / "build/RISCV/gem5.opt")
parser.add_argument("--images", type=Path, default=root / "build")
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--ref-so", type=Path)
parser.add_argument("--case", action="append", choices=[
    "ordered-lr", "ordered-lw", "sc-invalidation", "race"])
parser.add_argument("--param", action="append", default=[])
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env["GCB_MULTI_CORE_RESTORER"] = ""
if args.ref_so:
    env["GCBV_MULTI_CORE_REF_SO"] = str(args.ref_so.resolve())
records = []
for case in args.case or ["ordered-lr", "ordered-lw", "sc-invalidation", "race"]:
    out = (args.out / case).resolve()
    out.mkdir(parents=True, exist_ok=True)
    command = [str(args.gem5.resolve()), "-d", str(out),
               str(repo / "configs/example/smt_idealkmhv3.py"), "--raw-cpt",
               f"--generic-rv-cpt={(args.images / (case + '.bin')).resolve()}",
               "--mem-size=16GB", "--maxinsts=4000000",
               "--abs-max-tick=100000000000"]
    if not args.ref_so:
        command.append("--disable-difftest")
    for param in args.param:
        command += ["--param", param]
    (out / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (out / "log.txt").open("w") as log:
        try:
            result = subprocess.run(command, env=env, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=120)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    text = (out / "log.txt").read_text(errors="replace")
    guest = re.findall(r"LR_PROBE[^\r\n]*", text)
    passed = (code == 0 and len(guest) == 1 and "errors=0 " in guest[0]
              and "m5_exit instruction" in text)
    record = {"case": case, "returncode": code, "passed": passed,
              "guest": guest, "out": str(out)}
    records.append(record)
    print(json.dumps(record), flush=True)
(args.out / "results.json").write_text(json.dumps(records, indent=2) + "\n")
raise SystemExit(0 if all(record["passed"] for record in records) else 1)
