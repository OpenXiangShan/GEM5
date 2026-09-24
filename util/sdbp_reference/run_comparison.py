# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Run paired, fixed-instruction LRU/SDBP checkpoint comparisons locally.

The output directory must not exist: commands, logs, manifests and stats are
kept together, and an earlier experiment is never silently overwritten.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import time


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_job(job):
    directory, command, env = job
    directory.mkdir(parents=True)
    (directory / "command.json").write_text(
        json.dumps(command, indent=2) + "\n"
    )
    start = time.monotonic()
    with (directory / "run.log").open("w") as log:
        result = subprocess.run(
            command, stdout=log, stderr=subprocess.STDOUT, env=env
        )
    elapsed = time.monotonic() - start
    log = (directory / "run.log").read_text(errors="replace")
    # A simulator exit code alone does not prove the measurement finished.
    complete = (
        result.returncode == 0
        and "a thread reached the max instruction count" in log
    )
    status = {
        "returncode": result.returncode,
        "completed": complete,
        "wall_seconds": elapsed,
    }
    (directory / "status.json").write_text(json.dumps(status, indent=2) + "\n")
    print(f"{directory.name}: {status}", flush=True)
    return complete


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--checkpoint-root", type=Path, required=True)
    parser.add_argument(
        "--checkpoint",
        action="append",
        required=True,
        help="Benchmark/point directory, repeat for each point",
    )
    parser.add_argument(
        "--ref-so", type=Path, default=os.environ.get("GCBV_REF_SO")
    )
    parser.add_argument(
        "--binary", type=Path, default=Path("build/RISCV/gem5.opt")
    )
    parser.add_argument("--config", default="configs/example/idealkmhv3.py")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--warmup", type=int, default=20_000_000)
    parser.add_argument("--measure", type=int, default=20_000_000)
    parser.add_argument("--sampler-assoc", type=int, default=6)
    parser.add_argument("--sampler-num", type=int, default=32)
    parser.add_argument("--level", choices=("l2", "l3"), default="l2")
    parser.add_argument(
        "--variants",
        nargs="+",
        default=["lru", "sdbp", "bypass"],
        choices=("lru", "sdbp", "bypass"),
    )
    parser.add_argument("--extra-arg", action="append", default=[])
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    binary = (repo / args.binary).resolve()
    ref = args.ref_so.resolve() if args.ref_so else None
    if not ref or not ref.is_file():
        parser.error("Set GCBV_REF_SO or pass an existing --ref-so")
    if not binary.is_file():
        parser.error(f"Missing gem5 binary: {binary}")
    if args.jobs < 1 or args.warmup < 0 or args.measure < 1:
        parser.error(
            "jobs and measure must be positive; warmup must be nonnegative"
        )
    if len(set(args.variants)) != len(args.variants):
        parser.error("Each variant may only be requested once")
    if len(set(args.checkpoint)) != len(args.checkpoint):
        parser.error("Each checkpoint may only be requested once")
    checkpoints = {}
    for point in args.checkpoint:
        files = list((args.checkpoint_root / point).glob("*.zstd"))
        if len(files) != 1:
            parser.error(
                f"Expected one .zstd checkpoint in {point}, got {len(files)}"
            )
        checkpoints[point] = str(files[0].resolve())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, GCBV_REF_SO=str(ref))
    # Normal GCPTs contain an embedded restorer.
    for key in ("GCB_RESTORER", "GCBV_RESTORER"):
        env.pop(key, None)
    manifest = {
        "base_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "ref_so": str(ref),
        "ref_sha256": sha256(ref),
        "config": args.config,
        "warmup": args.warmup,
        "measure": args.measure,
        "level": args.level,
        "sampler_assoc": args.sampler_assoc,
        "sampler_num": args.sampler_num,
        "checkpoints": checkpoints,
        "variants": args.variants,
        "extra_args": args.extra_arg,
        "invocation": sys.argv,
        "host": os.uname().nodename,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    (output / "tracked_changes.patch").write_bytes(
        subprocess.check_output(["git", "diff", "HEAD"], cwd=repo)
    )
    # git diff omits new files. Preserve untracked implementation/configuration
    # and experiment helpers as well, without collecting build artifacts.
    untracked = (
        subprocess.check_output(
            [
                "git",
                "ls-files",
                "--others",
                "--exclude-standard",
                "-z",
                "--",
                "src",
                "configs",
                "util",
            ],
            cwd=repo,
        )
        .decode()
        .split("\0")
    )
    with tarfile.open(output / "untracked_sources.tar.gz", "w:gz") as archive:
        for name in filter(None, untracked):
            archive.add(repo / name, arcname=name, recursive=False)
    jobs = []
    for point, checkpoint in checkpoints.items():
        for variant in args.variants:
            directory = output / f"{point.replace('/', '_')}__{variant}"
            policy = "lru" if variant == "lru" else "sdbp"
            other_level = "l3" if args.level == "l2" else "l2"
            command = [
                str(binary),
                f"--outdir={directory}",
                str(repo / args.config),
                f"--generic-rv-cpt={checkpoint}",
                "--enable-difftest",
                f"--difftest-ref-so={ref}",
                f"--warmup-insts-no-switch={args.warmup}",
                f"--maxinsts={args.warmup + args.measure}",
                f"--{args.level}-replacement-policy={policy}",
                f"--{other_level}-replacement-policy=lru",
                f"--sdbp-sampler-assoc={args.sampler_assoc}",
                f"--sdbp-sampler-num={args.sampler_num}",
            ]
            if variant == "bypass":
                command.append("--sdbp-enable-bypass")
            command.extend(args.extra_arg)
            jobs.append((directory, command, env))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(run_job, jobs))
    if not all(results):
        sys.exit("One or more runs failed; inspect status.json and run.log")


if __name__ == "__main__":
    main()
