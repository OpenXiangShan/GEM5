# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Run data-checking and latency regressions for L2 victim invalidation."""

import argparse
import concurrent.futures
import json
import re
from pathlib import Path
import subprocess
import time


def stats(path):
    text = path.read_text().split("---------- Begin Simulation Statistics")[-1]
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2:
            try:
                result[fields[0]] = float(fields[1])
            except ValueError:
                pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--loads", type=int, default=50000)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config = Path(__file__).with_name("l2_invalidate.py")
    cases = {
        "baseline": [],
        "clean": ["--enable", "--reads=100"],
        "mixed": ["--enable"],
        "functional": ["--enable", "--functional=50"],
        "slow_data": ["--enable", "--l1-data-latency=6"],
        "clean_eviction": ["--enable", "--clean-evict"],
        "trace_fast": ["--enable", "--testers=1", "--reads=35"],
        "trace_slow": [
            "--enable",
            "--testers=1",
            "--reads=35",
            "--l1-data-latency=6",
        ],
        "absent": ["--enable", "--l1-size=512B", "--l2-size=8KiB"],
        "pressure": [
            "--enable",
            "--write-buffers=1",
            "--testers=4",
            "--reads=35",
        ],
        "buffered_pressure": [
            "--enable",
            "--sliced-l2",
            "--write-buffers=1",
            "--testers=4",
            "--reads=35",
        ],
    }

    def run(item):
        name, options = item
        directory = output / name
        directory.mkdir()
        trace = name.startswith("trace_")
        command = [str(args.binary.resolve()), f"--outdir={directory}"]
        if trace:
            command.extend(
                ["--debug-flags=CacheRepl", "--debug-file=eviction.trace"]
            )
        command.extend(
            [str(config), f"--loads={2000 if trace else args.loads}", *options]
        )
        (directory / "command.json").write_text(json.dumps(command, indent=2))
        start = time.monotonic()
        with (directory / "run.log").open("w") as log:
            process = subprocess.Popen(
                command, cwd=repo, stdout=log, stderr=subprocess.STDOUT
            )
            (directory / "pid").write_text(str(process.pid))
            code = process.wait()
        record = {"returncode": code, "seconds": time.monotonic() - start}
        log = (directory / "run.log").read_text()
        record["passed"] = (
            code == 0 and "maximum number of loads reached" in log
        )
        if record["passed"]:
            values = stats(directory / "stats.txt")
            cache = (
                "system.l2_wrapper.slices.inner_cache"
                if ("--sliced-l2" in options)
                else "system.l2"
            )
            prefix = f"{cache}.evictionSnoop."
            record["counters"] = {
                key[len(prefix) :]: value
                for key, value in values.items()
                if key.startswith(prefix)
            }
            counts = record["counters"]
            if name == "buffered_pressure":
                record["passed"] &= (
                    values["system.l2_wrapper.slices.l1ReqEnterPipeFail"] > 0
                )
            if name == "baseline":
                record["passed"] &= sum(counts.values()) == 0
            else:
                if name == "absent":
                    record["passed"] &= counts["absent"] > 0
                else:
                    record["passed"] &= counts["clean"] > 0
                if name == "clean":
                    # MemTest samples the command from inclusive [0, 100].
                    # Even percent_reads=100 therefore emits rare writes.
                    record["passed"] &= counts["clean"] > 20 * counts["dirty"]
                elif name != "absent":
                    record["passed"] &= counts["dirtyResponses"] > 0
                if counts["absent"]:
                    record["passed"] &= (
                        counts["absentDelayTicks"] == 1000 * counts["absent"]
                    )
        if trace and record["passed"]:
            text = (directory / "eviction.trace").read_text()
            probes = {}
            for match in re.finditer(
                r"EvictL1Probe addr=(\S+) generated=(\d+) data=(\d+) "
                r"delay=(\d+) ready=(\d+)",
                text,
            ):
                addr, generated, dirty, delay, ready = match.groups()
                probes[addr, int(generated)] = (int(dirty), int(delay))
            expected_probe = 10000 if name == "trace_slow" else 6000
            checked = 0
            for match in re.finditer(
                r"EvictL1Data addr=(\S+) generated=(\d+) ready=(\d+)", text
            ):
                addr, generated, ready = match.groups()
                generated, ready = int(generated), int(ready)
                dirty, delay = probes[addr, generated]
                if delay == expected_probe:
                    record["passed"] &= ready - generated >= delay + 2000
                    checked += 1
            record["stable_dirty_timing_samples"] = checked
            record["passed"] &= checked > 0
        (directory / "status.json").write_text(json.dumps(record, indent=2))
        print(name, json.dumps(record), flush=True)
        return name, record

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        results = dict(pool.map(run, cases.items()))
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if not all(item["passed"] for item in results.values()):
        raise SystemExit("A data/latency regression failed; inspect run.log")


if __name__ == "__main__":
    main()
