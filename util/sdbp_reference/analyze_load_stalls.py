# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Export both phases and validate zero-issue load-stall diagnostic counters."""

import argparse
import csv
import json
from pathlib import Path
import re


PREFIX = "system.cpu.scheduler."


def read_blocks(path):
    text = path.read_text()
    blocks = re.findall(
        r"---------- Begin Simulation Statistics ----------.*?"
        r"---------- End Simulation Statistics[^\n]*",
        text,
        re.S,
    )
    result = []
    for block in blocks:
        counters = {}
        for line in block.splitlines():
            fields = line.split()
            if len(fields) < 2 or fields[0].startswith("-"):
                continue
            try:
                counters[fields[0]] = float(fields[1])
            except ValueError:
                continue
        result.append((block, counters))
    return result


def vector(counters, name):
    prefix = PREFIX + name + "::"
    return {
        key[len(prefix) :]: value
        for key, value in counters.items()
        if key.startswith(prefix) and key[len(prefix) :] != "total"
    }


def validate(counters):
    masks = vector(counters, "loadStallDepthMask")
    masks = {int(key): value for key, value in masks.items()}
    assert len(masks) == 32, "Missing depth mask bins"
    checks = {
        "any_load": (
            sum(value for mask, value in masks.items() if mask),
            counters[PREFIX + "memstall_any_load"],
        ),
    }
    for name, bits in [("l1miss", 2), ("l2miss", 6), ("l3miss", 14)]:
        checks[name] = (
            sum(value for mask, value in masks.items() if mask & bits == bits),
            counters[PREFIX + "memstall_" + name],
        )
    residual = sum(
        value for mask, value in masks.items() if mask and not mask & 2
    )
    checks["residual"] = (
        residual,
        counters[PREFIX + "memstall_any_load"]
        - counters[PREFIX + "memstall_l1miss"],
    )
    for name, expected in [
        ("loadStallStateMask", sum(masks.values())),
        ("loadStallOldestPC", checks["any_load"][1]),
        ("loadStallNoDepth1StateMask", residual),
        ("loadStallNoDepth1OldestDepthState", residual),
        ("loadStallNoDepth1OldestPC", residual),
    ]:
        checks[name] = (sum(vector(counters, name).values()), expected)
    for name, (actual, expected) in checks.items():
        assert actual == expected, (name, actual, expected)
    return {
        "checks_passed": list(checks),
        "scheduler_zero_issue_cycles": sum(masks.values()),
        "iq_zero_issue_cycles": counters["system.cpu.iq.numIssuedDist::0"],
        "scheduler_minus_iq_zero_issue_cycles": sum(masks.values())
        - counters["system.cpu.iq.numIssuedDist::0"],
        "no_depth1_cycles": residual,
        "pc_overflow_cycles": vector(counters, "loadStallOldestPC").get(
            "overflow", 0
        ),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = args.output
    manifest = json.loads((root / "manifest.json").read_text())
    summary = {}
    for directory in sorted(root.glob("*__*/")):
        status = json.loads((directory / "status.json").read_text())
        assert status["completed"] and status["returncode"] == 0, directory
        blocks = read_blocks(directory / "stats.txt")
        # SIGUSR1 progress snapshots do not reset counters. Select only the
        # automatic phase-end dumps, and retain every raw block in stats.txt.
        phase_blocks = [
            block
            for block in blocks
            if abs(block[1]["simInsts"] - manifest["measure"]) <= 100
        ]
        assert manifest["warmup"] == manifest["measure"], "Equal phases required"
        assert len(phase_blocks) == 2, (directory, "expected two phase-end dumps")
        phases = {}
        for label, (block, counters), expected in zip(
            ["warmup", "measurement"],
            phase_blocks,
            [manifest["warmup"], manifest["measure"]],
        ):
            assert abs(counters["simInsts"] - expected) <= 100, directory
            phases[label] = validate(counters)
            phases[label]["simInsts"] = counters["simInsts"]
            phases[label]["cycles"] = counters["system.cpu.numCycles"]
            phases[label]["ipc"] = (
                counters["system.cpu.committedInsts"]
                / counters["system.cpu.numCycles"]
            )
            (directory / f"{label}-stats.txt").write_text(block + "\n")
            (directory / f"{label}-counters.json").write_text(
                json.dumps(counters, indent=2) + "\n"
            )
        (directory / "phases.json").write_text(
            json.dumps({"valid": True, "phases": phases}, indent=2) + "\n"
        )
        summary[directory.name] = phases
    assert len(summary) == 2, "Expected one LRU/SDBP pair"
    (root / "load-stall-validation.json").write_text(
        json.dumps(summary, indent=2) + "\n"
    )
    for phase in ["warmup", "measurement"]:
        data = [
            json.loads((directory / f"{phase}-counters.json").read_text())
            for directory in [
                root / "mcf_4541__lru",
                root / "mcf_4541__sdbp",
            ]
        ]
        with (root / f"{phase}-counter-diff.csv").open("w") as stream:
            writer = csv.writer(stream)
            writer.writerow(["counter", "lru", "sdbp", "delta"])
            for key in sorted(data[0].keys() | data[1].keys()):
                old, new = (counters.get(key, 0) for counters in data)
                writer.writerow([key, old, new, new - old])
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
