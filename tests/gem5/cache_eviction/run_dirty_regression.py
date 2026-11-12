# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Run data and state regressions for retained L2 dirty responsibility."""

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="riscv64-linux-gnu-gcc")
    parser.add_argument("--loads", type=int, default=20000)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    tests = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    compiler = [
        args.compiler,
        "-nostdlib",
        "-static",
        "-march=rv64g",
        "-mabi=lp64d",
        "-Wl,-e,_start",
    ]
    guests = {}
    for name, source, options in (
        ("phases", "dirty_responsibility.S", []),
        ("bypass_owner", "bypass_dirty_owner.S", []),
        ("mixed", "bypass_workload.c", ["-O2", "-fno-builtin"]),
        (
            "deferred",
            "bypass_workload.c",
            ["-O2", "-fno-builtin", "-DLINES=256"],
        ),
        ("readonly", "bypass_readonly.S", []),
        ("readonly_fit", "bypass_readonly.S", ["-DLINES=512"]),
    ):
        guest = output / name
        subprocess.run(
            [*compiler, *options, str(tests / source), "-o", str(guest)],
            check=True,
        )
        guests[name] = guest
    manifest = {
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "guests": {
            name: hashlib.sha256(path.read_bytes()).hexdigest()
            for name, path in guests.items()
        },
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    jobs = []
    for name, options in (
        ("phase_timing", ["--invalidate"]),
        ("phase_timing_auto", []),
        ("phase_timing_legacy", ["--legacy", "--invalidate"]),
        ("phase_atomic", ["--mode=atomic"]),
        ("phase_atomic_legacy", ["--mode=atomic", "--legacy"]),
    ):
        jobs.append(
            (
                name,
                [
                    str(tests / "dirty_responsibility.py"),
                    str(guests["phases"]),
                    *options,
                ],
                "latest-data checks PASS",
            )
        )
    for mode in ("timing", "atomic"):
        jobs.append(
            (
                f"bypass_owner_{mode}",
                [
                    str(tests / "dirty_responsibility.py"),
                    str(guests["bypass_owner"]),
                    "--bypass",
                    f"--mode={mode}",
                    *(["--invalidate"] if mode == "timing" else []),
                ],
                "latest-data checks PASS",
            )
        )
        for name, guest, options in (
            ("bypass", "mixed", ["--bypass"]),
            (
                "readonly",
                "readonly",
                ["--bypass", "--read-only", "--l1-writeback-clean"],
            ),
            (
                "readonly_fit",
                "readonly_fit",
                [
                    "--read-only",
                    "--l1-writeback-clean",
                    "--max-memory-reads=800",
                ],
            ),
            (
                "readonly_fit_bypass",
                "readonly_fit",
                ["--bypass", "--read-only", "--max-memory-reads=800"],
            ),
            ("sdbp", "mixed", []),
            ("lru", "mixed", ["--replacement=lru"]),
            ("legacy", "mixed", ["--cache-hierarchy-policy=legacy"]),
            (
                "legacy_bypass",
                "mixed",
                ["--cache-hierarchy-policy=legacy", "--bypass"],
            ),
            ("deferred", "deferred", ["--deferred"]),
            ("deferred_bypass", "deferred", ["--deferred", "--bypass"]),
        ):
            jobs.append(
                (
                    f"se_{mode}_{name}",
                    [
                        str(tests / "hierarchy_bypass.py"),
                        str(guests[guest]),
                        f"--mode={mode}",
                        "--llc-exclusive",
                        *options,
                        *(
                            ["--l2-evict-invalidate-l1"]
                            if mode == "timing"
                            else []
                        ),
                    ],
                    "Bypass sampler and writeback checks PASS",
                )
            )
    for name, options in (
        ("mixed", []),
        ("functional", ["--functional=30"]),
        ("buffered", ["--sliced-l2"]),
        ("lower_snoop", ["--peer"]),
        ("lower_snoop_auto", ["--peer"]),
    ):
        jobs.append(
            (
                f"memtest_{name}",
                [
                    str(tests / "l2_invalidate.py"),
                    f"--loads={args.loads}",
                    "--reads=50",
                    "--testers=1",
                    "--l1-size=2KiB",
                    "--l2-size=8KiB",
                    "--write-buffers=1",
                    "--cache-hierarchy-policy=xiangshan_single_core",
                    *options,
                    *([] if name == "lower_snoop_auto" else ["--enable"]),
                ],
                "maximum number of loads reached",
            )
        )

    def run(job):
        name, options, success = job
        directory = output / name
        directory.mkdir()
        command = [str(binary), f"--outdir={directory}"]
        if name == "memtest_lower_snoop_auto":
            command.extend(
                [
                    "--debug-flags=CacheRepl",
                    "--debug-file=retained.trace",
                ]
            )
        command.extend(options)
        (directory / "command.json").write_text(
            json.dumps(command, indent=2) + "\n"
        )
        start = time.monotonic()
        with (directory / "run.log").open("w") as log:
            process = subprocess.Popen(
                command,
                cwd=repo,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            (directory / "pid").write_text(str(process.pid))
            try:
                code = process.wait(timeout=300)
            except subprocess.TimeoutExpired:
                process.kill()
                code = process.wait()
        text = (directory / "run.log").read_text(errors="replace")
        record = {
            "returncode": code,
            "seconds": time.monotonic() - start,
            "passed": code == 0 and success in text,
        }
        (directory / "status.json").write_text(
            json.dumps(record, indent=2) + "\n"
        )
        print(name, json.dumps(record), flush=True)
        if not record["passed"]:
            print("\n".join(text.splitlines()[-15:]), flush=True)
        return name, record

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        results = dict(pool.map(run, jobs))
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    if not all(result["passed"] for result in results.values()):
        raise SystemExit(
            "Dirty-responsibility regression failed; inspect run.log"
        )


if __name__ == "__main__":
    main()
