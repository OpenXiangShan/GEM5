# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

"""Run matched checkpoint difftest with L2 victim invalidation off/on."""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ref-so", type=Path, required=True)
    parser.add_argument(
        "--checkpoint", type=Path, action="append", required=True
    )
    parser.add_argument("--warmup", type=int, default=2000000)
    parser.add_argument("--measure", type=int, default=3000000)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    binary, ref = args.binary.resolve(), args.ref_so.resolve()
    checkpoints = [path.resolve() for path in args.checkpoint]
    for path in [binary, ref, *checkpoints]:
        if not path.is_file():
            parser.error(f"Missing input: {path}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = {
        "commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "binary": str(binary),
        "binary_sha256": digest(binary),
        "ref_so": str(ref),
        "ref_sha256": digest(ref),
        "warmup": args.warmup,
        "measure": args.measure,
        "memory": "SimpleMemory",
        "checkpoints": {str(path): digest(path) for path in checkpoints},
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2))
    (output / "implementation.patch").write_bytes(
        subprocess.check_output(["git", "diff", "HEAD"], cwd=repo)
    )
    env = dict(os.environ, GCBV_REF_SO=str(ref))
    for key in ("GCB_RESTORER", "GCBV_RESTORER", "GCBH_RESTORER"):
        env.pop(key, None)

    def run(item):
        checkpoint, enabled = item
        name = f"{checkpoint.parent.parent.name}_{checkpoint.parent.name}"
        name += "__invalidate" if enabled else "__baseline"
        directory = output / name
        directory.mkdir()
        command = [
            str(binary),
            f"--outdir={directory}",
            str(repo / "configs/example/kmhv3.py"),
            "--mem-type=SimpleMemory",
            f"--generic-rv-cpt={checkpoint}",
            "--enable-difftest",
            f"--difftest-ref-so={ref}",
            f"--warmup-insts-no-switch={args.warmup}",
            f"--maxinsts={args.warmup + args.measure}",
        ]
        if enabled:
            command.append("--l2-evict-invalidate-l1")
        (directory / "command.json").write_text(json.dumps(command, indent=2))
        start = time.monotonic()
        with (directory / "run.log").open("w") as log:
            process = subprocess.Popen(
                command,
                cwd=repo,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            (directory / "pid").write_text(str(process.pid))
            print(f"START {name} pid={process.pid}", flush=True)
            code = process.wait()
        log = (directory / "run.log").read_text(errors="replace")
        result = {
            "returncode": code,
            "seconds": time.monotonic() - start,
            "completed": code == 0
            and "a thread reached the max instruction count" in log,
        }
        (directory / "status.json").write_text(json.dumps(result, indent=2))
        print(f"DONE {name}: {json.dumps(result)}", flush=True)
        return name, result

    jobs = [
        (path, enabled) for path in checkpoints for enabled in (False, True)
    ]
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        results = dict(pool.map(run, jobs))
    (output / "results.json").write_text(json.dumps(results, indent=2))
    if not all(result["completed"] for result in results.values()):
        raise SystemExit("A checkpoint regression failed; inspect run.log")


if __name__ == "__main__":
    main()
