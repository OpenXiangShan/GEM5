#!/usr/bin/env python3
"""Verify that every audited SPEC17 pair differs only in selected model settings."""
import argparse
import json
from pathlib import Path

import analyze_spec17_tea_dip as joint

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path)
parser.add_argument("before")
parser.add_argument("after")
parser.add_argument("--output", type=Path, required=True)
parser.add_argument(
    "--expected-changes",
    type=Path,
    required=True,
    help="JSON section/parameter mapping to exact [before, after] values.",
)
args = parser.parse_args()
labels = [args.before, args.after]
audits = [
    json.loads((args.root / (label + "-audit.json")).read_text())
    for label in labels
]
assert all(a["passed"] for a in audits)
assert audits[0]["profile_sha256"] == audits[1]["profile_sha256"]
indexes = [
    {point["point"]: point for point in audit["points"]} for audit in audits
]
assert indexes[0].keys() == indexes[1].keys()
archives = [
    Path(
        json.loads((args.root / (label + "-state.json")).read_text())["runs"][
            label
        ]["archive"]
    )
    for label in labels
]
dependencies = [
    json.loads(
        (args.root / "dependencies" / (label + "-dram.json")).read_text()
    )
    for label in labels
]
assert dependencies[0]["sha256"] == dependencies[1]["sha256"]
for data in dependencies:
    assert (
        joint.sha256(args.root / "dependencies" / data["snapshot"])
        == data["sha256"]
    )
expected = {
    (section, key): tuple(values)
    for section, params in json.loads(
        args.expected_changes.read_text()
    ).items()
    for key, values in params.items()
}
rows = []
for point in sorted(indexes[0]):
    files = [
        archive / "spec_all" / point / "m5out/config.ini"
        for archive in archives
    ]
    hashes = [joint.sha256(file) for file in files]
    assert all(
        hashes[i] == indexes[i][point]["sha256"]["config.ini"]
        for i in range(2)
    ), point
    before, after = [joint.config(file) for file in files]
    assert set(before.sections()) == set(after.sections()), point
    for ini, dependency in zip([before, after], dependencies):
        assert (
            ini["system.mem_ctrls"]["configFile"]
            == dependency["original_path"]
        ), point
    changes = {
        (section, key): (before[section].get(key), after[section].get(key))
        for section in before.sections()
        for key in before[section].keys() | after[section].keys()
        if before[section].get(key) != after[section].get(key)
    }
    changes.pop(("system.mem_ctrls", "configFile"), None)
    assert changes == expected, (point, changes)
    rows.append(
        dict(
            point=point,
            before_config_sha256=hashes[0],
            after_config_sha256=hashes[1],
        )
    )
proof = dict(
    passed=True,
    before_run=audits[0]["run_id"],
    after_run=audits[1]["run_id"],
    pairs=len(rows),
    changes=[
        dict(section=s, parameter=k, before=a, after=b)
        for (s, k), (a, b) in expected.items()
    ],
    dram_config_sha256=dependencies[0]["sha256"],
    rows=rows,
    tool_sha256=joint.sha256(Path(__file__)),
)
args.output.write_text(json.dumps(proof, indent=2))
print(
    json.dumps(
        {
            key: proof[key]
            for key in [
                "passed",
                "before_run",
                "after_run",
                "pairs",
                "dram_config_sha256",
            ]
        },
        indent=2,
    )
)
