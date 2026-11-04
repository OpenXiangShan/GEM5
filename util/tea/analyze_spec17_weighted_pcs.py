#!/usr/bin/env python3
"""Trace weighted suite gains/regressions to stateful TEA and DIP PCs."""
import argparse
import json
from pathlib import Path
import subprocess

import analyze_spec17_tea_dip as joint

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("comparison", type=Path)
parser.add_argument("before", type=Path)
parser.add_argument("after", type=Path)
parser.add_argument("--before-archive", type=Path, required=True)
parser.add_argument("--after-archive", type=Path, required=True)
parser.add_argument("--benchmarks", nargs="+", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument(
    "--addr2line",
    default="llvm-addr2line",
    help="Address-to-source executable.",
)
parser.add_argument(
    "--objdump",
    default="riscv64-unknown-linux-gnu-objdump",
    help="RISC-V disassembler executable.",
)
parser.add_argument(
    "--application-elf",
    action="append",
    default=[],
    metavar="BENCHMARK=PATH",
    help="Use a provenance-verified application ELF for the named benchmark.",
)
args = parser.parse_args()
application_elf = {
    name: Path(path)
    for name, path in (value.split("=", 1) for value in args.application_elf)
}
comparison = json.loads(args.comparison.read_text())
suites = [json.loads(path.read_text()) for path in [args.before, args.after]]
assert comparison["passed"] and all(s["passed"] for s in suites)
assert [comparison["before_run"], comparison["after_run"]] == [
    s["run_id"] for s in suites
]
assert (
    suites[0]["profile_sha256"]
    == suites[1]["profile_sha256"]
    == comparison["profile_sha256"]
)
assert suites[0]["ref_sha256"] == suites[1]["ref_sha256"]
indexes = [{row["point"]: row for row in suite["points"]} for suite in suites]
points = {}
for benchmark in args.benchmarks:
    row = next(
        row
        for row in comparison["benchmarks"]
        if row["benchmark"] == benchmark
    )
    for key in ["largest_weighted_improvement", "largest_weighted_regression"]:
        point = row[key]
        if point is not None:
            points[point["point"]] = point
rows = []
for name, weight in points.items():
    folders = [
        archive / "spec_all" / name / "m5out"
        for archive in [args.before_archive, args.after_archive]
    ]
    windows = [joint.window(folder, True) for folder in folders]
    before, after = [window[0] for window in windows]
    assert before["TEA_state_PC_available"] and after["TEA_state_PC_available"]
    for index, window in zip(indexes, windows):
        assert window[0]["sha256"] == index[name]["sha256"], name
        assert window[0]["CPI"] == index[name]["CPI"], name
    changes = []
    a, b = [window[1] for window in windows]
    assert a.sections() == b.sections()
    for section in a.sections():
        for key in a[section].keys() | b[section].keys():
            if a[section].get(key) != b[section].get(key):
                values = [a[section].get(key), b[section].get(key)]
                if (section, key) == ("system.mem_ctrls", "configFile"):
                    assert joint.sha256(Path(values[0])) == joint.sha256(
                        Path(values[1])
                    )
                else:
                    changes.append(
                        dict(
                            section=section,
                            parameter=key,
                            before=values[0],
                            after=values[1],
                        )
                    )
    dip0, tea0 = windows[0][2:]
    dip1, tea1 = windows[1][2:]
    dip = {key: dip1[key] - dip0[key] for key in dip0.keys() | dip1.keys()}
    tea = {
        key: tea1[key] - tea0[key]
        for key in tea0.keys() | tea1.keys()
        if isinstance(key, tuple)
    }
    selected = set()
    for values in [dip, tea]:
        for state in {state for pc, state in values}:
            keys = sorted(
                (
                    key
                    for key in values
                    if key[1] == state and values[key] != 0
                ),
                key=lambda key: (values[key], key),
            )
            selected.update(pc for pc, state in keys[:3] + keys[-3:])
    workload = indexes[0][name]["workload"]
    checkpoint = Path(windows[0][1]["system"]["gcpt_file"])
    profile = checkpoint.parents[3]
    app_elf = application_elf.get(
        weight["benchmark"],
        profile
        / "elf"
        / (workload.split("_rate_", 1)[0] + "_rate_refrate.elf"),
    )
    image_manifest = json.loads(
        (profile / "manifest" / (workload + ".json")).read_text()
    )
    kernel_elf = profile / image_manifest["kernel"]["vmlinux"]
    pcs = sorted(selected)
    symbols = {}
    executable = {}
    elf_for_pc = {pc: kernel_elf if pc >= 1 << 63 else app_elf for pc in pcs}
    for elf in set(elf_for_pc.values()):
        selected_pcs = [pc for pc in pcs if elf_for_pc[pc] == elf]
        headers = subprocess.run(
            ["llvm-readelf", "-l", str(elf)],
            text=True,
            capture_output=True,
            check=True,
        ).stdout
        spans = []
        for line in headers.splitlines():
            fields = line.split()
            if fields and fields[0] == "LOAD" and "E" in fields[6:-1]:
                spans.append(
                    (
                        int(fields[2], 16),
                        int(fields[2], 16) + int(fields[5], 16),
                    )
                )
        executable.update(
            {
                pc: any(start <= pc < end for start, end in spans)
                for pc in selected_pcs
            }
        )
        output = subprocess.run(
            [
                args.addr2line,
                "-e",
                str(elf),
                "-f",
                "-C",
                *[hex(pc) for pc in selected_pcs],
            ],
            text=True,
            capture_output=True,
            check=True,
        ).stdout.splitlines()
        assert len(output) == 2 * len(selected_pcs)
        symbols.update(
            {
                pc: (output[2 * i], output[2 * i + 1])
                if executable[pc]
                else (None, None)
                for i, pc in enumerate(selected_pcs)
            }
        )
    pc_rows = []
    for pc in pcs:
        elf = elf_for_pc[pc]
        output = subprocess.run(
            [
                args.objdump,
                "-d",
                "--no-show-raw-insn",
                f"--start-address={pc}",
                f"--stop-address={pc+8}",
                str(elf),
            ],
            text=True,
            capture_output=True,
            check=True,
        ).stdout
        instructions = [
            line.strip()
            for line in output.splitlines()
            if line.lstrip().startswith(f"{pc:x}:")
        ]
        pc_rows.append(
            dict(
                pc=hex(pc),
                function=symbols[pc][0],
                location=symbols[pc][1],
                elf=str(elf),
                executable_address=executable[pc],
                instruction=instructions[0]
                if instructions and executable[pc]
                else None,
                TEA_delta_CPI={
                    state: tea.get((pc, state), 0)
                    for state in joint.TEA_STATES
                },
                DIP_delta_CPI={
                    state: dip.get((pc, state), 0)
                    for state in joint.DIP_STATES
                },
            )
        )
    rows.append(
        dict(
            **weight,
            before=before,
            after=after,
            CPI_delta=after["CPI"] - before["CPI"],
            TEA_delta_CPI={
                state: after["TEA_CPI"][state] - before["TEA_CPI"][state]
                for state in joint.TEA_STATES
            },
            DIP_delta_CPI={
                state: after["DIP_CPI"][state] - before["DIP_CPI"][state]
                for state in joint.DIP_STATES
            },
            PCs=pc_rows,
            config_changes=changes,
            elf_sha256={
                str(elf): joint.sha256(elf) for elf in set(elf_for_pc.values())
            },
        )
    )
    print(
        name,
        "speedup_pct",
        weight["speedup_pct"],
        "weighted_delta_CPI",
        weight["weighted_delta_CPI"],
        flush=True,
    )
proof = dict(
    passed=True,
    before_run=suites[0]["run_id"],
    after_run=suites[1]["run_id"],
    points=rows,
    comparison_sha256=joint.sha256(args.comparison),
    tool_sha256=joint.sha256(Path(__file__)),
    scope=(
        "Largest weighted gain and regression per requested benchmark "
        "from audited suites. PC state changes describe different "
        "observation points, not dynamic dependency edges."
    ),
)
args.output.write_text(json.dumps(proof, indent=2))
