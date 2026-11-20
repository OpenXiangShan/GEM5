#!/usr/bin/env python3
"""Run the fixed Matrix correctness suite from a shared bin directory."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import resource
import subprocess
import sys
import time


REPO = Path(__file__).resolve().parents[4]
IMAGE_DIR = Path("/nfs/share/gem5_ci/matrix")
REFERENCE = Path("/nfs/share/gem5_ci/ref/matrix/riscv64-nemu-interpreter-so")
IMAGES = {
    "gemm_i8_f32": "gemm_i8_f32-m512-k256-n512-test.bin",
    "flash_attention": "flash_attention-q256-kv512-d128-test.bin",
    "softmax": "softmax-fp32-n65536-test.bin",
    "quantize_f32_i8": "quantize_f32_i8-n65536-block32-test.bin",
    "rms_norm": "rms_norm-fp32-n4096-test.bin",
    "gemv": "gemv-fp32-1024x1024-test.bin",
    "matmul": "matmul-fp32-m128-k256-n128-test.bin",
    "add": "add-fp32-n65536-test.bin",
    "mul": "mul-fp32-n65536-test.bin",
    "div": "div-fp32-n65536-test.bin",
    "silu": "silu-fp32-n65536-test.bin",
    "transpose": "transpose-fp32-4x128x256-test.bin",
    "embedding": "embedding-fp32-v1024-d256-i128-test.bin",
}


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n")


def select_images(directory, cases=None):
    selected = set(cases) if cases else set(IMAGES)
    unknown = selected - IMAGES.keys()
    if unknown:
        raise ValueError("Unknown CI cases: " + ", ".join(sorted(unknown)))
    images = [
        {
            "id": name,
            "image": directory / filename,
            "matrix_required": name in ("gemm_i8_f32", "flash_attention"),
        }
        for name, filename in IMAGES.items()
        if name in selected
    ]
    missing = [
        str(item["image"]) for item in images if not item["image"].is_file()
    ]
    if missing:
        raise ValueError("Missing CI images: " + ", ".join(missing))
    return images


def evaluate(log, item, returncode, timed_out):
    failures = []
    if timed_out:
        failures.append("wall-clock timeout")
    if returncode != 0:
        failures.append(f"simulator exit {returncode}")
    numerical = re.search(r"\[matrix-ci\] result=(PASS|FAIL) .*", log)
    if not numerical or numerical[1] != "PASS":
        failures.append("numerical correctness verdict missing or failed")
    if not re.search(r"--- 1 passed, 0 failed, 0 skipped ---", log):
        failures.append("single-case completion summary missing")
    guest_completed = bool(
        re.search(r"Exiting @ tick \d+ because m5_exit instruction encountered", log)
    )
    if not guest_completed:
        failures.append("normal guest completion missing")
    if "Simulated exit code not 0" in log:
        failures.append("guest returned nonzero")
    diff_initialized = "NEMU register state: 1544 bytes" in log
    if not diff_initialized:
        failures.append("compatible NEMU difftest initialization missing")
    if re.search(r"(?:panic:|fatal:|Difftest failed|BAD TRAP)", log):
        failures.append("simulator/difftest failure reported")
    dispatch = re.findall(
        r"\[matrix-ci-dispatch\] reference=0 capabilities=(\d+) .*?status=(\d+)",
        log,
    )
    required = 3 if item["matrix_required"] else 1
    if not dispatch or any(
        int(c) & required != required or int(s) != 0 for c, s in dispatch
    ):
        failures.append("required implementation was not selected")
    summaries = []
    for line in log.splitlines():
        if "MatrixDiff committed=" in line:
            summaries.append(
                {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
            )
    pending = ("pendingChecks", "pendingOwners", "pendingMemory")
    required_counts = {
        "committed", "submitted", "completed", "checked", "memorySent",
        "memoryCompleted", "mma", *pending,
    }
    checks_complete = bool(summaries) and all(
        required_counts <= s.keys()
        and s["submitted"] == s["completed"] == s["checked"]
        and s["memorySent"] == s["memoryCompleted"]
        and all(s[k] == 0 for k in pending)
        for s in summaries
    )
    if not summaries:
        failures.append("Matrix completion/checking summary missing")
    elif not checks_complete:
        failures.append("Matrix work or memory checks incomplete")
    if item["matrix_required"] and not any(
        s.get("mma", 0) > 0 for s in summaries
    ):
        failures.append("no Matrix MMA was checked")
    divergence = re.search(r".*NEMU PC:.*GEM5 PC:.*inst:.*", log)
    diff_failed = bool(
        re.search(r"Difftest failed|Matrix difftest ended with", log)
    )
    difftest = (
        "failed"
        if diff_failed
        else "passed"
        if diff_initialized and guest_completed and checks_complete
        else "incomplete"
    )
    return {
        "passed": not failures,
        "failures": failures,
        "application_correctness": numerical[1] == "PASS"
        if numerical
        else None,
        "instruction_difftest": difftest,
        "guest_completed": guest_completed,
        "first_divergence": divergence[0] if divergence else None,
        "numerical_verdict": numerical[0] if numerical else None,
        "matrix_difftest": summaries,
    }


def run(args):
    # Abort diagnostics belong in logs; avoid large guest core dumps.
    _, core_limit = resource.getrlimit(resource.RLIMIT_CORE)
    resource.setrlimit(resource.RLIMIT_CORE, (0, core_limit))
    images = select_images(args.image_dir.resolve(), args.case)
    binary = args.gem5.resolve()
    reference = args.reference.resolve()
    for path in (binary, reference):
        if not path.is_file():
            raise FileNotFoundError(path)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    def run_one(item):
        case_dir = output / "runs" / item["id"]
        case_dir.mkdir(parents=True, exist_ok=True)
        invocation = [
            str(binary),
            "--listener-mode=off",
            "-d",
            str(case_dir / "m5out"),
            str(REPO / "configs/example/kmhv2_matrix.py"),
            "--matrix",
            "--enable-difftest",
            "--raw-cpt",
            "--maxinsts=0",
            "--warmup-insts-no-switch=0",
            f"--generic-rv-cpt={item['image']}",
            f"--difftest-ref-so={reference}",
        ]
        if item["matrix_required"]:
            invocation.append("--classic-l2")
        save(case_dir / "command.json", invocation)
        started = time.monotonic()
        timed_out = False
        with (case_dir / "sim.log").open("w") as stream:
            process = subprocess.Popen(
                invocation, cwd=REPO, stdout=stream, stderr=subprocess.STDOUT
            )
            try:
                returncode = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                returncode = process.returncode
        log = (case_dir / "sim.log").read_text(errors="replace")
        result = dict(
            evaluate(log, item, returncode, timed_out),
            id=item["id"],
            image=str(item["image"]),
            returncode=returncode,
            wall_seconds=time.monotonic() - started,
            log=str(case_dir / "sim.log"),
        )
        save(case_dir / "result.json", result)
        print(
            f"{item['id']}: {'PASS' if result['passed'] else 'FAIL'} "
            f"({result['wall_seconds']:.1f}s) {result['failures']}",
            flush=True,
        )
        return result

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(executor.map(run_one, images))
    save(output / "validation-results.json", results)
    return 0 if all(r["passed"] for r in results) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    runner = subparsers.add_parser("run")
    runner.add_argument("--output", type=Path, required=True)
    runner.add_argument(
        "--gem5", type=Path, default=REPO / "build/RISCV_MATRIX/gem5.opt"
    )
    runner.add_argument("--reference", type=Path, default=REFERENCE)
    runner.add_argument("--image-dir", type=Path, default=IMAGE_DIR)
    runner.add_argument("--jobs", type=int, default=2)
    runner.add_argument("--timeout", type=int, default=1800)
    runner.add_argument("--case", action="append")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("--jobs and --timeout must be positive")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
