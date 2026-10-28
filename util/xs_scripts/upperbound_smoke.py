#!/usr/bin/env python3
"""Run bounded correctness and activation checks for upper-bound probes."""

import argparse
import concurrent.futures
import configparser
import json
import os
from pathlib import Path
import re
import subprocess


PRESETS = {
    "baseline": [],
    "backend": ["--upperbound-backend"],
    "predict2": [
        "--param=system.cpu[0].branchPred.idealPredictionsPerCycle=2",
        "--param=system.cpu[0].branchPred.idealNoOverrideBubbles=True",
    ],
    "fetch2": [
        "--param=system.cpu[0].idealInstructionFetch=True",
        "--param=system.cpu[0].enableTwoFetch=True",
    ],
}
PRESETS["frontend"] = PRESETS["predict2"] + PRESETS["fetch2"]
PRESETS["combined"] = PRESETS["backend"] + PRESETS["frontend"]


def last_stats(path):
    text = path.read_text()
    blocks = re.findall(
        r"Begin Simulation Statistics(.*?)End Simulation Statistics",
        text, re.S)
    if not blocks:
        raise ValueError(f"No complete stats block in {path}")
    values = {}
    for line in blocks[-1].splitlines():
        fields = line.split()
        if len(fields) > 1:
            try:
                values[fields[0]] = float(fields[1])
            except ValueError:
                pass
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path("build/RISCV/gem5.opt"))
    parser.add_argument("--image", type=Path, default=Path(
        "/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin"))
    parser.add_argument("--checkpoint", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--presets", nargs="+", choices=PRESETS,
                        default=list(PRESETS))
    parser.add_argument("--maxinsts", type=int, default=300000)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    image = args.image.resolve()
    if not binary.is_file() or not image.is_file():
        parser.error("Binary and input must exist")
    ref = subprocess.check_output(
        ["python3", "util/nemu_ref/resolve.py", "normal-dedup"],
        cwd=root, text=True).strip()
    env = {**os.environ, "GCBV_REF_SO": ref}

    def run(name):
        output = args.out / name
        output.mkdir(exist_ok=True)
        command = [str(binary), f"--outdir={output}",
                   "configs/example/kmhv3.py",
                   f"--generic-rv-cpt={image}", "--enable-mem-dedup",
                   "--warmup-insts-no-switch=0",
                   f"--maxinsts={args.maxinsts}", *PRESETS[name]]
        if not args.checkpoint:
            command.append("--raw-cpt")
        (output / "command.json").write_text(json.dumps(command, indent=2))
        with (output / "run.log").open("w") as log:
            result = subprocess.run(command, cwd=root, env=env,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    timeout=900)
        text = (output / "run.log").read_text(errors="replace")
        assert result.returncode == 0, (name, result.returncode)
        assert not re.search(r"panic:|fatal:|Assertion .* failed|"
                             r"Difftest.*(?:failed|mismatch)", text, re.I), name
        stats = last_stats(output / "stats.txt")
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.optionxform = str
        config.read(output / "config.ini")
        assert config.getboolean("system.cpu", "enable_difftest"), name
        assert stats["system.cpu.committedInsts"] > 0, name
        if "backend" in name or name == "combined":
            assert config.getint("system.cpu", "numROBEntries") == 640
            assert not config.getboolean("system.cpu.scheduler", "enableSpMT")
            queues = config.get("system.cpu.scheduler", "IQs").split()
            names = [config.get(queue, "name") for queue in queues]
            assert names.count("ld3") == 1, names
        if name in ["predict2", "frontend", "combined"]:
            assert stats["system.cpu.branchPred.idealSecondPredictionEnqueues"] > 0
        if name in ["fetch2", "frontend", "combined"]:
            assert stats["system.cpu.fetch.idealInstructionSupplies"] > 0
            assert stats["system.cpu.fetch.twoFetchSuccesses"] > 0
        selected = {k: v for k, v in stats.items() if
                    k in ["system.cpu.committedInsts", "system.cpu.numCycles",
                          "system.cpu.ipc", "system.cpu.fetch.twoFetchSuccesses"] or
                    "idealSecondPrediction" in k or "idealInstruction" in k or
                    "loadPipeAccepted" in k or "lateLoadCancel" in k}
        record = {"preset": name, "exit_code": result.returncode,
                  "input": str(image), "ref": ref, "stats": selected,
                  "scope": "bounded correctness/activation smoke; not a SPEC score"}
        (output / "validation.json").write_text(json.dumps(record, indent=2))
        return record

    records = []
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run, name): name for name in args.presets}
        for future in concurrent.futures.as_completed(futures):
            try:
                record = future.result()
                records.append(record)
                print(json.dumps({"preset": record["preset"], "passed": True}),
                      flush=True)
            except Exception as error:
                failures.append({"preset": futures[future], "error": str(error)})
                print(json.dumps(failures[-1]), flush=True)
    (args.out / "summary.json").write_text(json.dumps(
        {"records": records, "failures": failures}, indent=2))
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()
