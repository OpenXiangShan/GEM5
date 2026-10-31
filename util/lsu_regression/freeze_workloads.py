#!/usr/bin/env python3
"""Compile one immutable workload directory shared by both A/B runs."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from run_suite import DEFAULT_CASES

FROZEN_CASES = [*DEFAULT_CASES, "fs10_deep"]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
here = Path(__file__).resolve().parent
for name in FROZEN_CASES:
    d = out / name
    d.mkdir()
    cmd = ["riscv64-linux-gnu-gcc", "-static", "-Wl,--build-id=none"]
    if name.startswith("se"):
        if name == "se_smt":
            cmd += [
                "-nostdlib",
                "-march=rv64gc",
                "-mabi=lp64d",
                "-Wl,-Ttext=0x10000",
                str(here / "se_smt.S"),
            ]
        else:
            cmd += [
                "-O2",
                "-march=rv64gc",
                "-mabi=lp64d",
                "-DCASE=" + name[2:],
                str(here / "memory_cases.c"),
            ]
    else:
        cmd += [
            "-nostdlib",
            "-mabi=lp64d",
            "-mno-relax",
            "-fno-pic",
            "-fno-pie",
            "-Wl,--no-relax",
        ]
        if name.startswith("vector"):
            cmd += [
                "-march=rv64gcv",
                "-Wl,-Ttext=0x80000000",
                str(here / "vector_cross.S"),
            ]
            if name == "vector_mask":
                cmd += ["-DTEST_MASKED"]
        elif name.startswith("store"):
            cmd += [
                "-march=rv64gc",
                "-DWARM=" + str(int(name == "store_warm")),
                "-DEXPECT_BAD=0",
                "-Wl,-T," + str(here / "ordinary_store.ld"),
                str(here / "ordinary_store.S"),
            ]
        else:
            case_id = (
                "10"
                if name == "fs10_deep"
                else name[2:]
                if name.startswith("fs")
                else "0"
            )
            cmd += [
                "-march=rv64gc",
                "-DCASE=" + case_id,
                "-DEXPECT_BAD=" + str(int(name == "negative")),
                "-Wl,-T," + str(here / "ordinary_store.ld"),
                str(here / "paths.S"),
            ]
    cmd += ["-o", str(d / "test.elf")]
    subprocess.run(cmd, check=True)
    subprocess.run(
        [
            "riscv64-linux-gnu-objcopy",
            "-O",
            "binary",
            str(d / "test.elf"),
            str(d / "test.bin"),
        ],
        check=True,
    )
manifest = {
    str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
    for p in sorted(out.rglob("*"))
    if p.is_file()
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print("Frozen", len(FROZEN_CASES), "cases in", out)
